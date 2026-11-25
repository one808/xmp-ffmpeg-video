// xmp-ffmpeg.cpp - XMPlay input plugin, powered by FFmpeg - VIDEO WINDOW variant
//
// Plays (almost) anything FFmpeg can DECODE: all audio formats, plus the audio
// stream of any video file - and for files with a video stream, a borderless
// video window opens automatically (resizable, double-click/F for fullscreen,
// ESC to leave fullscreen or hide the window). Audio-only files keep the
// window hidden.
//
// Everything (libavformat/libavcodec/libswresample/libswscale/libavutil +
// external codec libs) is linked STATICALLY into this DLL - no FFmpeg DLLs.
// File I/O goes through XMPlay's XMPFILE routines (net streaming, archives).
//
// Sync: the video timeline follows XMPlay's playback position
// (XMPFUNC_STATUS->GetTime). Demuxing stays single-threaded on the audio
// path; video packets go to a queue that the render thread decodes.
//
// Built for Windows XP+ (see .github/workflows/build.yml).

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include <windows.h>
#include "xmpin.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#define PLUGIN_NAME "xmp-ffmpeg"
// version is injected by scripts/build-plugin.sh from the VERSION file
#ifndef XMP_FFMPEG_VERSION
#define XMP_FFMPEG_VERSION "1.1"
#endif
#define PLUGIN_VER  XMP_FFMPEG_VERSION

static XMPFUNC_IN     *xmpfin;
static XMPFUNC_MISC   *xmpfmisc;
static XMPFUNC_FILE   *xmpffile;
static XMPFUNC_TEXT   *xmpftext;
static XMPFUNC_STATUS *xmpfstatus;
static DWORD xmpver;

struct FFMPEG {
	XMPFILE file;
	AVFormatContext *fmt;
	AVIOContext *io;
	AVCodecContext *dec;
	SwrContext *swr;
	AVFrame *frame;
	AVPacket *pkt;
	int stream;          // audio stream index
	int vstream;         // video stream index (-1 = none)
	AVCodecContext *vdec;// video decoder (used by the render thread!)
	double pos;          // current output position (seconds)
	double duration;     // seconds, 0 = unknown
	DWORD bitrate;       // kbps (0 = unknown)
	BOOL seekable, netfile, sent_eof, eof;
	volatile BOOL demux_eof; // av_read_frame hit EOF (read by render thread)
	float *outbuf;       // interleaved float32 staging buffer
	DWORD outcap;        // capacity in floats
	DWORD outlen, outp;  // floats valid / consumed
	int recheck;         // lazy video discovery countdown
	BOOL direct_net;     // opened over the network by FFmpeg itself
	const char *codec;   // audio codec short name
	int vw, vh;
	const char *video_codec;
	// audio packets the video top-up read but could not forward (decoder
	// input full). The audio thread drains these before its own reads.
	AVPacket *pend_a[8];
	int pend_n;
	// seek fast-forward pending: FF_SetPosition sets it to the target; the
	// audio thread runs the read-and-skip on its next FF_Process call (never
	// the UI thread - a network download must not freeze XMPlay)
	double ff_target;
};
static FFMPEG *current;
static CRITICAL_SECTION demux_lock; // guards av_read_frame/av_seek_frame/send

// video queue buffering: FF_Process tops the queue up to this depth so the
// decode thread never starves (the audio-demand feed alone delivers video
// packets ~just-in-time; see the post-seek judder, v1.2.43 logs)
#define VQ_TARGET 60
#define VQ_LOOKAHEAD_MAX 64

// --------------------------------------------------------------- video state

struct VPKT { VPKT *next; AVPacket *pkt; };

struct VIDEO {
	CRITICAL_SECTION lock;
	volatile BOOL active;      // a video stream is attached
	volatile BOOL running;     // render thread alive
	DWORD threadid;
	HANDLE thread;
	// demuxer/decoder refs (freed by the audio side, used under lock)
	AVFormatContext *fmt;
	AVCodecContext *vdec;
	AVRational tb;
	int vstream;               // attached video stream index
	// packet queue (producers: audio thread / consumer: render thread)
	VPKT *qhead, *qtail;
	int qcount;
	BOOL v_eof_sent, demux_eof_seen;
	// rendering
	SwsContext *sws;
	int sws_w, sws_h, sws_fmt;
	AVFrame *vframe;
	BOOL have_frame;           // decoded frame in vframe, waiting for its time
	double frame_pts;
	BYTE *dib;                 // BGRA DIBits
	BITMAPINFO bmi;
	int dibw, dibh;
	// playback clock: captured on XMPlay's thread, interpolated here. The
	// render thread must NEVER call XMPlay functions (deadlock risk).
	double clock_base;
	DWORD clock_tick;
	double last_pts, frame_dur;   // pts fallback for streams without stamps
	AVRational sar;               // non-square pixel ratio (display aspect)
	AVRational frame_sar;         // per-frame override when present
	BOOL have_new;                // a decoded frame is waiting for FF_VisRenderDC
	BOOL vis_blank;               // vis area currently black (cleared on stop)
	volatile BOOL seeking;         // seek catch-up in progress (show black meanwhile)
};
static VIDEO video;

static void video_enqueue(AVPacket *pkt); // takes ownership
static void video_flush();
static void video_attach(FFMPEG *f);
static void video_detach();

// ------------------------------------------------------------- diagnostic log
// Always-on lightweight log so field problems are diagnosable from the file.
// Location: %TEMP%\xmp-ffmpeg-video.log

static void logmsg(const char *fmt, ...)
{
	char dir[300];
	if (!GetTempPathA(300, dir)) return;
	strcat(dir, "xmp-ffmpeg-video.log");
	FILE *f = fopen(dir, "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

// route FFmpeg's own log into ours - network/TLS failures then show up with
// their real cause (403, handshake, DNS, ...)
static void ff_log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
	if (level > AV_LOG_INFO) return;
	char dir[300];
	if (!GetTempPathA(300, dir)) return;
	strcat(dir, "xmp-ffmpeg-video.log");
	FILE *f = fopen(dir, "a");
	if (!f) return;
	fputs("  ff: ", f);
	vfprintf(f, fmt, vl);
	fclose(f);
}

// ---------------------------------------------------------------- XMPFILE I/O

static int io_read(void *opaque, uint8_t *buf, int size)
{
	FFMPEG *f = (FFMPEG*)opaque;
	while (1) {
		DWORD n = xmpffile->Read(f->file, buf, size);
		if (n) return (int)n;
		if (!xmpffile->NetIsActive(f->file)) return AVERROR_EOF;
		if (xmpfmisc->CheckCancel()) return AVERROR_EXIT;
		if (!xmpffile->NetPreBuf(f->file)) Sleep(10); // still connected, waiting for data
	}
}

static int64_t io_seek(void *opaque, int64_t offset, int whence)
{
	FFMPEG *f = (FFMPEG*)opaque;
	if (whence == AVSEEK_SIZE) return (int64_t)xmpffile->GetSize64(f->file);
	int64_t base = 0;
	if (whence == SEEK_CUR) base = (int64_t)xmpffile->Tell64(f->file);
	else if (whence == SEEK_END) base = (int64_t)xmpffile->GetSize64(f->file);
	else if (whence != SEEK_SET) return -1;
	offset += base;
	if (offset < 0) return -1;
	if (xmpver >= 0x03080200) {
		if (!xmpffile->Seek64(f->file, (QWORD)offset)) return -1;
	} else {
		if (!xmpffile->Seek(f->file, (DWORD)offset)) return -1;
	}
	return offset;
}

// ------------------------------------------------------------------- helpers

static void FreeFF(FFMPEG *f)
{
	if (!f) return;
	if (f->swr) swr_free(&f->swr);
	if (f->dec) avcodec_free_context(&f->dec);
	if (f->vdec) avcodec_free_context(&f->vdec);
	if (f->fmt) avformat_close_input(&f->fmt); // CUSTOM_IO: does not free our AVIO
	if (f->io) { // (absent in direct-net mode - close_input freed everything)
		// avio_context_free does NOT free the IO buffer - and avio may have
		// reallocated it, so free io->buffer (per the official avio_read_callback
		// example). Without this, every file open/close leaks 64KB.
		av_freep(&f->io->buffer);
		avio_context_free(&f->io);
	}
	if (f->frame) av_frame_free(&f->frame);
	if (f->pkt) av_packet_free(&f->pkt);
	for (int i = 0; i < 8; i++) if (f->pend_a[i]) av_packet_free(&f->pend_a[i]);
	free(f->outbuf);
	free(f);
}

// bytes-per-sample code for XMPFORMAT.res: 1=8-bit 2=16-bit 3=24-bit 4=float
static WORD res_code(AVSampleFormat fmt, int rawbits)
{
	if (rawbits > 0) {
		if (rawbits <= 8) return 1;
		if (rawbits <= 16) return 2;
		if (rawbits <= 24) return 3;
		return 4;
	}
	int b = av_get_bytes_per_sample(fmt);
	if (b <= 1) return 1;
	if (b <= 2) return 2;
	if (b == 3) return 3;
	return 4;
}

static BOOL ensure_outbuf(FFMPEG *f, DWORD floats)
{
	if (floats <= f->outcap) return TRUE;
	float *p = (float*)realloc(f->outbuf, floats * sizeof(float));
	if (!p) return FALSE;
	f->outbuf = p;
	f->outcap = floats;
	return TRUE;
}

// open + analyse a file (used by GetFileInfo and Open)
static FFMPEG *InitFF(const char *filename, XMPFILE file)
{
	FFMPEG *f = (FFMPEG*)calloc(1, sizeof(FFMPEG));
	if (!f) return 0;
	f->file = file;
	f->vstream = -1;
	DWORD type = xmpffile->GetType(file);
	f->seekable = (type != XMPFILE_TYPE_NETSTREAM);
	f->netfile = (type >= XMPFILE_TYPE_NETFILE);

	// For URLs let FFmpeg fetch them with its own networking stack. XMPlay's
	// net layer resolves playlists like m3u8 down to the AUDIO stream before
	// handing them over (it is an audio player), which hides the video track
	// completely - opening the URL directly keeps every stream. Fall back to
	// the XMPFILE custom IO when that fails.
	if (!file || (filename && strstr(filename, "://"))) {
		AVFormatContext *nfmt = avformat_alloc_context();
		if (nfmt) {
			nfmt->probesize = 32 * 1024 * 1024;
			nfmt->max_analyze_duration = 10 * 1000000;
			AVDictionary *opts = NULL;
			av_dict_set(&opts, "user_agent",
				"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
				"(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36", 0);
			av_dict_set(&opts, "reconnect", "1", 0);
			av_dict_set(&opts, "reconnect_streamed", "1", 0);
			av_dict_set(&opts, "reconnect_delay_max", "5", 0);
			// No certificate verification: HLS segment hosts routinely use
			// self-signed/private-CA certs (mbedtls would reject every one of
			// them - the earlier 'segment failed, skipping' storm). Same
			// policy XMPlay's own networking uses. These options are
			// inherited by every nested segment fetch.
			av_dict_set(&opts, "verify", "0", 0);
			av_dict_set(&opts, "tls_verify", "0", 0);
			int nr = avformat_open_input(&nfmt, filename, NULL, &opts);
			av_dict_free(&opts);
			if (nr >= 0) {
				f->fmt = nfmt;
				f->direct_net = TRUE;
				logmsg("direct-net open ok: format=%s file=%s",
					nfmt->iformat ? nfmt->iformat->name : "?", filename);
			} else {
				logmsg("direct-net failed %d, using XMPFILE layer", nr);
			}
		}
	}
	const int IOBUF = 64 * 1024;
	if (!f->fmt && !file) { logmsg("open: no XMPFILE and direct open failed"); goto fail; }
	if (!f->fmt) {
		unsigned char *iobuf = (unsigned char*)av_malloc(IOBUF);
		f->io = avio_alloc_context(iobuf, IOBUF, 0, f, io_read, NULL, f->seekable ? io_seek : NULL);
		if (!f->io) { av_free(iobuf); free(f); return 0; }
		f->io->seekable = f->seekable ? AVIO_SEEKABLE_NORMAL : 0;

		f->fmt = avformat_alloc_context();
		if (!f->fmt) goto fail;
		f->fmt->pb = f->io;
		f->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
	}
	// pass the filename/URL: it is only a hint for our custom IO, but nested
	// formats like HLS resolve their segment URLs against it (s->url)
	f->fmt->probesize = 32 * 1024 * 1024;      // stubborn streams (TS/HLS)
	f->fmt->max_analyze_duration = 10 * 1000000;
	// pass the filename/URL: it is only a hint for our custom IO, but nested
	// formats like HLS resolve their segment URLs against it (s->url)
	if (!f->direct_net) {
		int ior = avformat_open_input(&f->fmt, filename, NULL, NULL);
		if (ior < 0) {
			logmsg("open_input FAILED %d file=%s", ior, filename ? filename : "?");
			goto fail;
		}
	}
	logmsg("open ok: format=%s streams=%u file=%s",
		f->fmt->iformat ? f->fmt->iformat->name : "?", f->fmt->nb_streams, filename ? filename : "?");
	if (avformat_find_stream_info(f->fmt, NULL) < 0) logmsg("find_stream_info failed (continuing)");
	for (unsigned li = 0; li < f->fmt->nb_streams; li++) {
		AVCodecParameters *lp = f->fmt->streams[li]->codecpar;
		logmsg("  stream %u: type=%d codec=%d %dx%d", li, lp->codec_type, lp->codec_id, lp->width, lp->height);
	}

	{ // pick the audio stream
		int s = av_find_best_stream(f->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
		if (s < 0) goto fail; // no audio in this file
		f->stream = s;
	}

	{ // open the audio decoder
		AVStream *st = f->fmt->streams[f->stream];
		const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
		if (!c) goto fail;
		f->dec = avcodec_alloc_context3(c);
		if (!f->dec) goto fail;
		if (avcodec_parameters_to_context(f->dec, st->codecpar) < 0) goto fail;
		if (avcodec_open2(f->dec, c, NULL) < 0) goto fail;
		const AVCodecDescriptor *d = avcodec_descriptor_get(st->codecpar->codec_id);
		f->codec = d ? d->name : c->name;
	}

	{ // pick + open the video stream (for the video window)
		int vs = -1;
		for (unsigned i = 0; i < f->fmt->nb_streams; i++) {
			AVCodecParameters *p = f->fmt->streams[i]->codecpar;
			if (p->codec_type == AVMEDIA_TYPE_VIDEO && !(f->fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
				vs = (int)i;
				break;
			}
		}
		logmsg("video scan: candidate stream idx=%d", vs);
		if (vs >= 0) {
			AVStream *st = f->fmt->streams[vs];
			const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
			if (c) {
				f->vdec = avcodec_alloc_context3(c);
				if (f->vdec && avcodec_parameters_to_context(f->vdec, st->codecpar) >= 0
					&& avcodec_open2(f->vdec, c, NULL) >= 0) {
					f->vstream = vs;
					f->vw = st->codecpar->width;
					f->vh = st->codecpar->height;
					const AVCodecDescriptor *d = avcodec_descriptor_get(st->codecpar->codec_id);
					f->video_codec = d ? d->name : c->name;
					logmsg("video decoder OPEN: %s %dx%d", f->video_codec, f->vw, f->vh);
				} else if (f->vdec) {
					logmsg("video decoder open FAILED");
					avcodec_free_context(&f->vdec);
				}
			} else {
				logmsg("no video decoder for codec %d", st->codecpar->codec_id);
			}
		} else {
			logmsg("no video stream found at open (may appear later)");
		}
	}

	if (f->dec->sample_rate <= 0 || f->dec->ch_layout.nb_channels <= 0) goto fail;

	// unknown channel layout -> assume default order so swresample accepts it
	if (f->dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
		av_channel_layout_default(&f->dec->ch_layout, f->dec->ch_layout.nb_channels);

	// decode -> interleaved float32 at the source rate/layout (XMPlay resamples
	// and downmixes to the output device itself)
	if (swr_alloc_set_opts2(&f->swr, &f->dec->ch_layout, AV_SAMPLE_FMT_FLT, f->dec->sample_rate,
			&f->dec->ch_layout, f->dec->sample_fmt, f->dec->sample_rate, 0, NULL) < 0) goto fail;
	if (swr_init(f->swr) < 0) goto fail;

	f->frame = av_frame_alloc();
	f->pkt = av_packet_alloc();
	if (!f->frame || !f->pkt) goto fail;

	{ // length + bitrate
		AVStream *st = f->fmt->streams[f->stream];
		if (f->fmt->duration != AV_NOPTS_VALUE && f->fmt->duration > 0)
			f->duration = (double)f->fmt->duration / AV_TIME_BASE;
		else if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
			f->duration = st->duration * av_q2d(st->time_base);
		QWORD br = st->codecpar->bit_rate ? st->codecpar->bit_rate : f->fmt->bit_rate;
		f->bitrate = (DWORD)(br / 1000);
	}
	return f;
fail:
	FreeFF(f);
	return 0;
}

// tags as a flat "name\0value\0...\0\0" buffer (XMPlay convention)
static const char *tag_keys[] = { "title", "artist", "album", "date", "track", "genre", "comment" };

static char *BuildTags(FFMPEG *f)
{
	DWORD cap = 256, len = 0;
	char *buf = (char*)xmpfmisc->Alloc(cap);
	if (!buf) return 0;
	#define TAGAPPEND(s) do { \
		int n = (int)strlen(s) + 1; \
		while (len + n + 1 > cap) { cap *= 2; buf = (char*)xmpfmisc->ReAlloc(buf, cap); if (!buf) return 0; } \
		memcpy(buf + len, s, n); len += n; } while (0)

	// "filetype" intentionally NOT set (Ian: it renders misleading like "raw"/
	// "Ogg" - XMPlay's extension-based name is better)
	for (int i = 0; i < (int)(sizeof(tag_keys) / sizeof(tag_keys[0])); i++) {
		AVDictionaryEntry *e = av_dict_get(f->fmt->streams[f->stream]->metadata, tag_keys[i], NULL, 0);
		if (!e) e = av_dict_get(f->fmt->metadata, tag_keys[i], NULL, 0);
		if (e && e->value && e->value[0]) {
			TAGAPPEND(tag_keys[i]);
			TAGAPPEND(e->value); // metadata is UTF-8 = XMPlay native text on NT
		}
	}
	buf[len] = 0; // terminating NULL
	#undef TAGAPPEND
	return buf;
}

// ================================================================ video layer

// -- queue (called from the audio decode path; takes ownership of pkt)

static void video_enqueue(AVPacket *pkt)
{
	if (!video.active || video.vstream < 0) { av_packet_free(&pkt); return; }
	VPKT *n = (VPKT*)malloc(sizeof(VPKT));
	if (!n) { av_packet_free(&pkt); return; }
	n->next = 0;
	n->pkt = pkt;
	EnterCriticalSection(&video.lock);
	if (video.qcount > 600) { // bounded queue: drop the new packet instead of
		LeaveCriticalSection(&video.lock); // growing without limit
		av_packet_free(&pkt);
		free(n);
		return;
	}
	if (video.qtail) video.qtail->next = n; else video.qhead = n;
	video.qtail = n;
	video.qcount++;
	LeaveCriticalSection(&video.lock);
}

static void video_flush()
{
	EnterCriticalSection(&video.lock);
	VPKT *p = video.qhead;
	while (p) { VPKT *n = p->next; av_packet_free(&p->pkt); free(p); p = n; }
	video.qhead = video.qtail = 0;
	video.qcount = 0;
	video.have_frame = FALSE;
	video.v_eof_sent = video.demux_eof_seen = FALSE;
	video.last_pts = -1; // new timeline after a seek
	if (video.vdec) avcodec_flush_buffers(video.vdec);
	LeaveCriticalSection(&video.lock);
}

// -- playback clock (captured on XMPlay's thread, interpolated on ours)

static void video_clock_capture(double t)
{
	if (!(t > -1e6 && t < 1e7)) return; // NaN/garbage: keep interpolating
	EnterCriticalSection(&video.lock);
	video.clock_base = t;
	video.clock_tick = GetTickCount();
	LeaveCriticalSection(&video.lock);
}

static double video_clock_now()
{
	EnterCriticalSection(&video.lock);
	double t = video.clock_base + (double)(GetTickCount() - video.clock_tick) * 0.001;
	LeaveCriticalSection(&video.lock);
	return t;
}

// -- rendering

static BOOL video_make_dib(int w, int h)
{
	if (video.dib && video.dibw == w && video.dibh == h) return TRUE;
	free(video.dib);
	memset(&video.bmi, 0, sizeof(video.bmi));
	video.bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	video.bmi.bmiHeader.biWidth = w;
	video.bmi.bmiHeader.biHeight = -h; // top-down
	video.bmi.bmiHeader.biPlanes = 1;
	video.bmi.bmiHeader.biBitCount = 32;
	video.bmi.bmiHeader.biCompression = BI_RGB;
	video.dib = (BYTE*)malloc((size_t)w * h * 4);
	if (video.dib) memset(video.dib, 0, (size_t)w * h * 4); // black, not garbage
	video.dibw = w; video.dibh = h;
	return video.dib != 0;
}

// Scale the decoded frame in vframe into the shared DIB (video.dib). Caller
// holds video.lock. No GDI here - FF_VisRenderDC later blits this buffer into
// XMPlay's own visualisation area.
static void video_render_frame()
{
	AVFrame *fr = video.vframe;
	if (fr->width < 1 || fr->height < 1) return;
	if (!video_make_dib(fr->width, fr->height)) return;
	if (!video.sws || video.sws_w != fr->width || video.sws_h != fr->height
		|| video.sws_fmt != fr->format) {
		if (video.sws) sws_freeContext(video.sws);
		video.sws = sws_getContext(fr->width, fr->height, (AVPixelFormat)fr->format,
			fr->width, fr->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
		video.sws_w = fr->width; video.sws_h = fr->height; video.sws_fmt = fr->format;
		if (!video.sws) return;
	}
	uint8_t *dst[4] = { video.dib, 0, 0, 0 };
	int ls[4] = { fr->width * 4, 0, 0, 0 };
	sws_scale(video.sws, fr->data, fr->linesize, 0, fr->height, dst, ls);
	video.have_new = TRUE; // a fresh frame is waiting for FF_VisRenderDC
}

// decode the frame(s) due at the playback position into video.dib.
// BOUNDED work per call: without a budget, a broken-timestamp stream (all
// frames "late") makes this loop decode forever - 100% CPU spin, starved audio.
static void video_tick()
{
	if (!video.active || !video.vdec) return;
	double t = video_clock_now();
	EnterCriticalSection(&video.lock);
	if (!video.active || !video.vdec) { LeaveCriticalSection(&video.lock); return; }
	int budget = 8; // max decode/feed iterations this tick
	int nstarve = 0, ndrop = 0;
	double shown_pts = -1;
	while (budget-- > 0) {
		if (!video.have_frame) {
			int ret = avcodec_receive_frame(video.vdec, video.vframe);
			if (ret == AVERROR(EAGAIN)) {
				// feed one packet (or the drain marker at demux EOF)
				if (video.qhead) {
					VPKT *n = video.qhead;
					video.qhead = n->next;
					if (!video.qhead) video.qtail = 0;
					video.qcount--;
					if (avcodec_send_packet(video.vdec, n->pkt) == AVERROR(EAGAIN)) {
						// decoder is full - put the packet back and drain first
						n->next = video.qhead;
						video.qhead = n;
						if (!video.qtail) video.qtail = n;
						video.qcount++;
						break;
					}
					av_packet_free(&n->pkt);
					free(n);
					continue;
				}
				if (video.demux_eof_seen && !video.v_eof_sent) {
					video.v_eof_sent = TRUE;
					avcodec_send_packet(video.vdec, NULL);
					continue;
				}
				nstarve++;
				break; // starved - wait for more packets
			}
			if (ret < 0) break; // drained or error
			// timestamp: real pts when present, else advance from the last one
			double pts;
			if (video.vframe->best_effort_timestamp != AV_NOPTS_VALUE) {
				pts = video.vframe->best_effort_timestamp * av_q2d(video.tb);
			} else {
				pts = video.last_pts + video.frame_dur;
			}
			if (pts > video.last_pts) video.last_pts = pts;
			video.frame_pts = pts;
			video.frame_sar = video.vframe->sample_aspect_ratio;
			video.have_frame = TRUE;
			continue;
		}
		// have a decoded frame: show it when due, drop when far too late.
		// While a seek catch-up is in progress the drop budget is unlimited:
		// stale frames must NOT be shown (they'd flash old content and clear
		// `seeking` early) - keep the area black until a frame in the target
		// window is shown.
		if (video.frame_pts > t + 0.06) break;   // in the future - wait
		if (video.frame_pts < t - 0.30 && (budget > 2 || video.seeking)) { // behind - drop
			video.have_frame = FALSE;
			ndrop++;
			continue;
		}
		video_render_frame(); // scale into video.dib (under lock); sets have_new
		shown_pts = video.frame_pts;
		if (video.seeking) video.seeking = FALSE; // a shown frame is within the
		                                         // clock window - catch-up done
		video.have_frame = FALSE;
		break; // one present per tick is enough
	}
	int qnow = video.qcount;
	LeaveCriticalSection(&video.lock);
	// 1/sec diagnostic heartbeat: queue depth + per-second starvation/drop
	// counts, used to diagnose the post-seek judder (%TEMP%\xmp-ffmpeg-video.log)
	{
		static int s_starved = 0, s_dropped = 0;
		static double s_shown_pts = -1;
		if (shown_pts > 0) s_shown_pts = shown_pts;
		s_starved += nstarve;
		s_dropped += ndrop;
		DWORD nowt = GetTickCount();
		static DWORD s_last = 0;
		if (!s_last) s_last = nowt;
		if (nowt - s_last >= 1000) {
			s_last = nowt;
			logmsg("vt: q=%d starved=%d dropped=%d last_shown=%.2f clock=%.2f",
				qnow, s_starved, s_dropped, s_shown_pts, t);
			s_starved = s_dropped = 0;
		}
	}
}

// -- window (B1 standalone borderless window: removed. The video now renders
// into XMPlay's own vis area - see FF_VisRenderDC; the render thread below
// only decodes into video.dib)

static DWORD WINAPI video_thread(LPVOID)
{
	// B2: no window of our own - just keep the latest frame decoded into
	// video.dib; XMPlay's vis area shows it via FF_VisRenderDC.
	while (1) {
		video_tick();
		// during a seek catch-up run at ~1kHz instead of 66Hz: the queued
		// stale frames (keyframe -> target) are then dropped at decoder
		// speed, so a local seek shows the target in ~0.1-0.3s instead of
		// churning through them at the 15ms cadence (~2s).
		Sleep(video.seeking ? 1 : 15);
	}
	return 0;
}

static void video_window_init()
{
	if (video.running) return;
	video.clock_base = 0;
	video.clock_tick = GetTickCount();
	video.thread = CreateThread(0, 0, video_thread, 0, 0, &video.threadid);
	video.running = TRUE;
}

// called after a successful Open: attach this file's video to the vis area
static void video_attach(FFMPEG *f)
{
	if (!f || f->vstream < 0) {
		video_detach();
		return;
	}
	video_flush();
	EnterCriticalSection(&video.lock);
	video.fmt = f->fmt;
	video.vdec = f->vdec;
	video.vstream = f->vstream;
	video.tb = f->fmt->streams[f->vstream]->time_base;
	video.last_pts = -1;
	video.frame_dur = 0.04;
	{
		AVRational fr = f->fmt->streams[f->vstream]->avg_frame_rate;
		if (fr.num > 0 && fr.den > 0) video.frame_dur = (double)fr.den / fr.num;
	}
	video.sar = av_guess_sample_aspect_ratio(f->fmt, f->fmt->streams[f->vstream], NULL);
	video.have_new = FALSE;
	video.vis_blank = FALSE;
	video.seeking = FALSE; // fresh track: not in a seek catch-up
	video.active = TRUE;
	LeaveCriticalSection(&video.lock);
	video_window_init(); // make sure the decoder thread is alive
}

// hide + disengage (Close / audio-only / teardown)
static void video_detach()
{
	EnterCriticalSection(&video.lock);
	video.active = FALSE;
	VPKT *p = video.qhead;
	while (p) { VPKT *n = p->next; av_packet_free(&p->pkt); free(p); p = n; }
	video.qhead = video.qtail = 0; video.qcount = 0;
	video.have_frame = FALSE;
	video.vdec = 0; video.fmt = 0;
	video.have_new = FALSE;
	video.vis_blank = FALSE;
	video.seeking = FALSE; // stop the fast catch-up cadence
	LeaveCriticalSection(&video.lock);
}

// ------------------------------------------------------- XMPlay vis callbacks
// B2: the video shows in XMPlay's OWN visualisation area. XMPlay opens/closes/
// fullscreen-s it (VisOpen/VisClose + the area's own double-click); we just
// hand it the latest decoded frame via FF_VisRenderDC. No window of our own.

static BOOL WINAPI FF_VisOpen(DWORD colors[3])
{
	logmsg("vis: open");
	video_window_init(); // ensure the decoder thread is alive
	EnterCriticalSection(&video.lock);
	video.vis_blank = FALSE; // new track/vis session: refresh resumes
	LeaveCriticalSection(&video.lock);
	return TRUE;
}

static void WINAPI FF_VisClose()
{
	logmsg("vis: close");
}

static void WINAPI FF_VisSize(HDC dc, SIZE *size)
{
	if (size) { size->cx = video.dibw ? video.dibw : 640; size->cy = video.dibh ? video.dibh : 480; }
}

static BOOL WINAPI FF_VisRender(DWORD *buf, SIZE size, DWORD flags)
{
	return FALSE; // we render through the DC variant
}

static BOOL WINAPI FF_VisRenderDC(HDC dc, SIZE size, DWORD flags)
{
	RECT rc = { 0, 0, size.cx, size.cy };

	// main-program-driven blank: when XMPlay has stopped/paused, its status face
	// says "not playing" -> show black so the last frame doesn't linger in the
	// vis area. (XMPlay itself only clears the vis AREA / main-window clock on
	// a full close - that part is not something the plugin controls.)
	// Per Ian (un4seen #21049): return true the first time we clear, then
	// false afterwards so XMPlay stops refreshing a static display.
	if (xmpfstatus && !xmpfstatus->IsPlaying()) {
		EnterCriticalSection(&video.lock);
		BOOL need_blank = !video.vis_blank;
		if (need_blank) video.vis_blank = TRUE;
		LeaveCriticalSection(&video.lock);
		if (need_blank) {
			FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
			return TRUE;
		}
		return FALSE;
	}

	// seek catch-up in progress: keep the area black so the stale frame from
	// before the seek doesn't linger; the render thread clears `seeking` the
	// moment it shows a frame within the target window
	{
		BOOL s;
		EnterCriticalSection(&video.lock);
		s = video.seeking;
		LeaveCriticalSection(&video.lock);
		if (s) {
			FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
			return TRUE;
		}
	}

	// playing: the display is live again, so the blank latch is stale
	EnterCriticalSection(&video.lock);
	video.vis_blank = FALSE;
	LeaveCriticalSection(&video.lock);

	// no video stream on this track (audio-only) -> keep it blank
	if (!video.dib || video.dibw < 1 || !video.vdec) {
		FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
		return TRUE;
	}

	EnterCriticalSection(&video.lock);
	if (!video.have_new) { LeaveCriticalSection(&video.lock); return FALSE; } // unchanged
	FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
	if (video.dibw >= 1 && video.dibh >= 1) {
		double sar = 1.0;
		AVRational fsar = video.frame_sar;
		if (fsar.num <= 0 || fsar.den <= 0) fsar = video.sar;
		if (fsar.num > 0 && fsar.den > 0) sar = (double)fsar.num / fsar.den;
		double va = (double)video.dibw / video.dibh * sar;
		double wa = (double)size.cx / (size.cy > 0 ? size.cy : 1);
		int w, h, x, y;
		if (wa > va) { h = size.cy; w = (int)(h * va + 0.5); x = (size.cx - w) / 2; y = 0; }
		else          { w = size.cx; h = (int)(w / va + 0.5); x = 0; y = (size.cy - h) / 2; }
		if (w < 1) w = 1;
		if (h < 1) h = 1;
		// smooth (filtered) scaling - the default GDI stretch jaggies badly,
		// especially when shrinking the video into a small vis area
		int oldmode = SetStretchBltMode(dc, HALFTONE);
		StretchDIBits(dc, x, y, w, h, 0, 0, video.dibw, video.dibh,
			video.dib, &video.bmi, DIB_RGB_COLORS, SRCCOPY);
		SetStretchBltMode(dc, oldmode);
	}
	video.have_new = FALSE;
	LeaveCriticalSection(&video.lock);
	return TRUE;
}

// ============================================================= XMPIN methods

static void WINAPI FF_About(HWND win)
{
	char msg[512];
	snprintf(msg, sizeof(msg), "%s %s\n\nPlays any audio format FFmpeg can decode,\n"
		"including the audio stream of video files,\n"
		"with an automatic borderless video window.\n\n"
		"Video window: drag to move, drag edges to resize,\n"
		"double-click or F/Enter = fullscreen, ESC = leave/hide.\n\n"
		"FFmpeg: %s\nXMPlay plugin interface %u",
		PLUGIN_NAME, PLUGIN_VER, av_version_info(), (unsigned)XMPIN_FACE);
	MessageBox(win, msg, "About " PLUGIN_NAME, MB_ICONINFORMATION);
}

// Content check: XMPlay calls this for files it cannot route by extension
// (and before opening) - we let avformat probe the actual data so anything
// with a real audio/video stream is claimed regardless of its file name.
static BOOL WINAPI FF_CheckFile(const char *filename, XMPFILE file)
{
	unsigned char buf[65536];
	DWORD n = xmpffile->Read(file, buf, sizeof(buf));
	xmpffile->Seek(file, 0); // rewind - the file must be usable afterwards
	if (n < 16) return FALSE;
	AVProbeData pd;
	memset(&pd, 0, sizeof(pd));
	pd.filename = filename ? filename : "";
	pd.buf = buf;
	pd.buf_size = (int)n;
	int score = 0;
	const AVInputFormat *fmt = av_probe_input_format2(&pd, TRUE, &score);
	logmsg("check: %s -> %s score=%d", filename ? filename : "?", fmt ? fmt->name : "none", score);
	return fmt && score >= 25; // AVPROBE_SCORE_RETRY
}

static DWORD WINAPI FF_GetFileInfo(const char *filename, XMPFILE file, float **length, char **tags)
{
	logmsg("cb: GetFileInfo(%s)", filename ? filename : "(null)");
	FFMPEG *f = InitFF(filename, file);
	if (!f) return 0;
	if (length && f->duration > 0) {
		float *lens = (float*)xmpfmisc->Alloc(sizeof(float));
		if (lens) {
			lens[0] = (float)f->duration;
			*length = lens;
		}
	}
	if (tags) *tags = BuildTags(f);
	FreeFF(f);
	return 1;
}

static DWORD WINAPI FF_Open(const char *filename, XMPFILE file)
{
	logmsg("cb: Open(%s, file=%p)", filename ? filename : "(null)", (void*)file);
	current = InitFF(filename, file);
	if (!current) return 0;
	current->pos = 0;
	current->recheck = 1; // first late-scan on the first Process call
	if (current->direct_net) current->seekable = TRUE;
	video_clock_capture(0);
	logmsg("OPEN: vstream=%d audio=%s dur=%.3f seekable=%d", current->vstream,
		current->codec ? current->codec : "?", current->duration, current->seekable);
	xmpfin->SetLength((float)(current->duration > 0 ? current->duration : -1), current->seekable);
	if (current->netfile)
		xmpffile->NetSetRate(file, current->bitrate ? current->bitrate * 125 : 32000);
	video_attach(current); // attach the vis video iff this file has a video stream
	// 2 = success and XMPlay may close its file: we did the I/O ourselves
	// (direct networking / no XMPFILE). This makes XMPlay forward real seek
	// positions for internet streams (Ian @ un4seen). The earlier HLS-only
	// 'return 1' workaround is gone - XMPlay now releases its HLS state when
	// Open returns 2 (un4seen forum #21049).
	return (current->direct_net || !file) ? 2 : 1;
}

static void WINAPI FF_Close()
{
	logmsg("cb: Close");
	video_detach(); // stop the render thread before freeing the video decoder
	EnterCriticalSection(&demux_lock);
	FFMPEG *f = current;
	current = 0;
	LeaveCriticalSection(&demux_lock);
	FreeFF(f);
}

static char *WINAPI FF_GetTags()
{
	logmsg("cb: GetTags");
	EnterCriticalSection(&demux_lock);
	FFMPEG *f = current;
	char *r = f ? BuildTags(f) : NULL;
	LeaveCriticalSection(&demux_lock);
	return r;
}

static void WINAPI FF_SetFormat(XMPFORMAT *form)
{
	logmsg("cb: SetFormat");
	FFMPEG *f = current;
	if (!f) return;
	form->rate = f->dec->sample_rate;
	form->chan = f->dec->ch_layout.nb_channels;
	form->res = res_code(f->dec->sample_fmt, f->dec->bits_per_raw_sample);
	form->chanmask = (f->dec->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) ? (WORD)f->dec->ch_layout.u.mask : 0;
}

static void WINAPI FF_GetInfoText(char *format, char *length)
{
	FFMPEG *f = current;
	if (!f) return;
	if (format) {
		format += sprintf(format, "%s", f->codec ? f->codec : "?");
		if (f->bitrate) format += sprintf(format, " - %ukb/s", (unsigned)f->bitrate);
		sprintf(format, " - %dhz", f->dec->sample_rate);
	}
}

static void WINAPI FF_GetGeneralInfo(char *buf)
{
	logmsg("cb: GetGeneralInfo");
	EnterCriticalSection(&demux_lock);
	FFMPEG *f = current;
	if (!f) { LeaveCriticalSection(&demux_lock); return; }
	const AVInputFormat *iformat = f->fmt->iformat;
	buf += sprintf(buf, "Format\t%s\r", iformat && iformat->long_name ? iformat->long_name : "?");
	buf += sprintf(buf, "Audio codec\t%s\r", f->codec ? f->codec : "?");
	if (f->vstream >= 0)
		buf += sprintf(buf, "Video\t%s %dx%d\r", f->video_codec ? f->video_codec : "?", f->vw, f->vh);
	if (f->bitrate) buf += sprintf(buf, "Bit rate\t%u kbps\r", (unsigned)f->bitrate);
	buf += sprintf(buf, "Sample rate\t%d hz\rChannels\t%d\r",
		f->dec->sample_rate, f->dec->ch_layout.nb_channels);
	LeaveCriticalSection(&demux_lock);
}

static void WINAPI FF_GetMessage(char *buf)
{
	logmsg("cb: GetMessage");
	EnterCriticalSection(&demux_lock);
	FFMPEG *f = current;
	if (!f) { LeaveCriticalSection(&demux_lock); return; }
	AVDictionary *md[2] = { f->fmt->streams[f->stream]->metadata, f->fmt->metadata };
	for (int i = 0; i < 2; i++) {
		AVDictionaryEntry *e = NULL;
		while ((e = av_dict_get(md[i], "", e, AV_DICT_IGNORE_SUFFIX)))
			buf = xmpfmisc->FormatInfoText(buf, e->key, e->value);
	}
	LeaveCriticalSection(&demux_lock);
}

// HLS/TS streams can expose their video track LATE (variants load over time).
// While no video is attached, rescan the stream list occasionally and attach
// as soon as one shows up.
static void video_late_scan(FFMPEG *f)
{
	if (f->vstream >= 0 || video.active) return;
	if (--f->recheck > 0) return;
	f->recheck = 200; // roughly every few seconds of audio
	for (unsigned i = 0; i < f->fmt->nb_streams; i++) {
		AVCodecParameters *p = f->fmt->streams[i]->codecpar;
		if (p->codec_type != AVMEDIA_TYPE_VIDEO) continue;
		const AVCodec *c = avcodec_find_decoder(p->codec_id);
		if (!c) continue;
		AVCodecContext *vdec = avcodec_alloc_context3(c);
		if (!vdec) return;
		if (avcodec_parameters_to_context(vdec, p) < 0 || avcodec_open2(vdec, c, NULL) < 0) {
			avcodec_free_context(&vdec);
			return;
		}
		EnterCriticalSection(&video.lock);
		f->vstream = (int)i;
		f->vdec = vdec;
		f->vw = p->width;
		f->vh = p->height;
		const AVCodecDescriptor *d = avcodec_descriptor_get(p->codec_id);
		f->video_codec = d ? d->name : c->name;
		LeaveCriticalSection(&video.lock);
		logmsg("late video attach: stream %u %s %dx%d", i, f->video_codec, f->vw, f->vh);
		video_attach(f);
		return;
	}
}

static DWORD WINAPI FF_Process(float *buffer, DWORD count)
{
	FFMPEG *f = current;
	if (!f) return 0;
	video_late_scan(f);
	// video clock: XMPlay's nominal track time. It tracks the audible audio
	// head; f->pos (what we've handed the device so far) leads it by the
	// audio device buffer (~0.4s here, and it wobbles 0.1->0.4 right after
	// a seek when the buffer drains and refills) - driving the picture off
	// f->pos makes the video rush ahead of the audio and stutter.
	if (video.active) video_clock_capture(xmpfstatus ? xmpfstatus->GetTime() : f->pos);
	{ // demand + clock heartbeat, 10/sec
		static DWORD lastbeat;
		DWORD now = GetTickCount();
		if (now - lastbeat >= 100) {
			lastbeat = now;
			logmsg("cb: count=%lu fpos=%.2f gettime=%.2f", (unsigned long)count,
				f->pos, xmpfstatus ? xmpfstatus->GetTime() : -1.0);
		}
	}
	EnterCriticalSection(&demux_lock);
	DWORD done = 0;
	int ch = f->dec->ch_layout.nb_channels;
	if (ch < 1) ch = 1;

	// deferred seek fast-forward (set by FF_SetPosition): skip packets up to
	// the seek target so audio + video start AT the target, not at the (often
	// far behind) keyframe. Audio packets are skipped (the decoder was
	// flushed), video packets enqueued for the render thread's quick
	// drop-catch-up. Bounded per call; on EAGAIN (m3u8 still downloading) the
	// fast-forward simply resumes on the next call.
	if (f->ff_target > 0 && !f->sent_eof && !f->eof) {
		double t = f->ff_target;
		if (video.active && f->vstream >= 0) {
			int guard = 0;
			while (guard++ < 400) {
				int rr = av_read_frame(f->fmt, f->pkt);
				if (rr < 0) {
					if (rr == AVERROR(EAGAIN)) break; // still downloading: retry next call
					f->ff_target = 0; // real EOF: give up the fast-forward
					break;
				}
				int si = f->pkt->stream_index;
				double cur = -1;
				BOOL saw_pts = FALSE;
				if (f->pkt->pts != AV_NOPTS_VALUE) {
					cur = (double)f->pkt->pts * av_q2d(f->fmt->streams[si]->time_base);
					saw_pts = TRUE;
				}
				if (guard == 1 && !saw_pts) { // no timestamps at all: can't fast-forward
					av_packet_unref(f->pkt);
					f->ff_target = 0;
					break;
				}
				if (si == f->vstream) {
					AVPacket *vp = av_packet_alloc();
					if (vp) { av_packet_move_ref(vp, f->pkt); video_enqueue(vp); }
					else av_packet_unref(f->pkt);
				} else {
					av_packet_unref(f->pkt); // audio before the target: skip
				}
				if (cur >= t) { f->ff_target = 0; break; } // reached the target
			}
		} else {
			f->ff_target = 0; // audio-only track: the demuxer seeks exactly, nothing to skip
		}
	}

	while (done < count) {
		if (f->outp < f->outlen) { // staged samples available
			DWORD n = count - done;
			if (n > f->outlen - f->outp) n = f->outlen - f->outp;
			memcpy(buffer + done, f->outbuf + f->outp, n * sizeof(float));
			f->outp += n;
			done += n;
			f->pos += (double)n / ((double)ch * f->dec->sample_rate);
			continue;
		}
		if (f->eof) break;

		int ret = avcodec_receive_frame(f->dec, f->frame);
		if (ret == 0) { // got a frame -> resample to interleaved float
			int maxn = swr_get_out_samples(f->swr, f->frame->nb_samples);
			if (maxn < 1) maxn = f->frame->nb_samples + 256;
			if (!ensure_outbuf(f, (DWORD)maxn * ch)) { f->eof = TRUE; break; }
			uint8_t *dst = (uint8_t*)f->outbuf;
			int got = swr_convert(f->swr, &dst, maxn,
				(const uint8_t**)f->frame->extended_data, f->frame->nb_samples);
			if (got < 0) got = 0;
			f->outlen = (DWORD)got * ch;
			f->outp = 0;
			continue;
		}
		if (ret == AVERROR_EOF || (ret < 0 && ret != AVERROR(EAGAIN))) {
			f->eof = TRUE; // drained or decode error
			break;
		}
		// decoder needs more input
		if (f->sent_eof) { f->eof = TRUE; break; }
		// drain audio packets held back by a full decoder input, in order,
		// ahead of any demuxer reads (keeps audio packet order intact)
		if (f->pend_n > 0) {
			AVPacket *pa = f->pend_a[0];
			int pr = avcodec_send_packet(f->dec, pa);
			if (pr == 0) {
				for (int i = 1; i < f->pend_n; i++) f->pend_a[i - 1] = f->pend_a[i];
				f->pend_n--;
				av_packet_free(&pa);
				continue; // that frame can come out of the decoder now
			}
			break; // still full: return what we have; XMPlay calls again
		}
		ret = av_read_frame(f->fmt, f->pkt);
		if (ret >= 0) {
			if (f->pkt->stream_index == f->vstream && video.active) {
				// hand the video packet to the render thread
				AVPacket *vp = av_packet_alloc();
				if (vp) { av_packet_move_ref(vp, f->pkt); video_enqueue(vp); }
				else av_packet_unref(f->pkt);
				continue;
			}
			if (f->pkt->stream_index != f->stream) {
				av_packet_unref(f->pkt); // other streams - skip
				continue;
			}
			{ int sr = avcodec_send_packet(f->dec, f->pkt);
			  if (sr == AVERROR(EAGAIN)) {
				  // decoder input full: hold the packet - the OLD unconditional
				  // unref here lost audio whenever this fired
				  AVPacket *keep = av_packet_alloc();
				  if (keep) {
					  av_packet_move_ref(keep, f->pkt);
					  if (f->pend_n < 8) f->pend_a[f->pend_n++] = keep; else av_packet_free(&keep);
				  } else av_packet_unref(f->pkt);
				  continue;
			  }
			  av_packet_unref(f->pkt); // decoder took its own ref (or send failed)
			}
			continue;
		}
		if (ret == AVERROR(EAGAIN)) continue;
		f->sent_eof = TRUE;
		f->demux_eof = TRUE;
		EnterCriticalSection(&video.lock);
		video.demux_eof_seen = TRUE;
		LeaveCriticalSection(&video.lock);
		avcodec_send_packet(f->dec, NULL); // end of input: drain the decoder
	}

	// top the video queue up to VQ_TARGET. The demand loop above only reads
	// what the audio request needs, so video packets arrive just-in-time
	// (the queue hovers at 0-2 and the decode thread starves, worse after a
	// seek when catch-up needs extra frames - the judder in the v1.2.43
	// logs: q=0 starved=40/s dropped=15/s). A ~1.6s buffer absorbs seek
	// catch-up and m3u8 segment stalls. Audio packets read on the way are
	// forwarded to the decoder, held in f->pend_a when its input is full.
	if (video.active && f->vstream >= 0 && !f->sent_eof && !f->eof) {
		int guard = 0;
		while (video.qcount < VQ_TARGET && guard++ < VQ_LOOKAHEAD_MAX
			&& !f->sent_eof && f->pend_n < 8) {
			int r2 = av_read_frame(f->fmt, f->pkt);
			if (r2 < 0) {
				if (r2 == AVERROR(EAGAIN)) { guard++; continue; }
				f->sent_eof = TRUE;
				f->demux_eof = TRUE;
				EnterCriticalSection(&video.lock);
				video.demux_eof_seen = TRUE;
				LeaveCriticalSection(&video.lock);
				avcodec_send_packet(f->dec, NULL); // end of input: drain the decoder
				break;
			}
			if (f->pkt->stream_index == f->vstream) {
				AVPacket *vp = av_packet_alloc();
				if (vp) { av_packet_move_ref(vp, f->pkt); video_enqueue(vp); }
				else av_packet_unref(f->pkt);
				continue;
			}
			if (f->pkt->stream_index != f->stream) { av_packet_unref(f->pkt); continue; }
			int sr = avcodec_send_packet(f->dec, f->pkt);
			if (sr == 0) { av_packet_unref(f->pkt); continue; }
			if (sr == AVERROR(EAGAIN)) {
				AVPacket *keep = av_packet_alloc();
				if (keep) { av_packet_move_ref(keep, f->pkt); f->pend_a[f->pend_n++] = keep; }
				else av_packet_unref(f->pkt);
				// held in pend (drained by the main loop) - keep topping up
				// the video queue; the loop condition caps pend at 8
				continue;
			}
			av_packet_unref(f->pkt); // send error: drop it
		}
	}
	LeaveCriticalSection(&demux_lock);
	return done;
}

static double WINAPI FF_GetGranularity()
{
	return 0.001;
}

static double WINAPI FF_SetPosition(DWORD pos)
{
	FFMPEG *f = current;
	if (!f) return -1;
	double t = pos * 0.001;
	logmsg("seek: begin pos=%lu t=%.3f", (unsigned long)pos, t);
	if (!f->seekable) { logmsg("seek: not seekable (pos=%lu t=%.3f)", (unsigned long)pos, t); return -1; }
	EnterCriticalSection(&demux_lock);

	// three strategies, in order; HLS is picky about which form it accepts
	int r = av_seek_frame(f->fmt, -1, (int64_t)(t * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
	if (r < 0) {
		AVStream *st = f->fmt->streams[f->stream];
		int64_t sts = (int64_t)(t / av_q2d(st->time_base));
		r = av_seek_frame(f->fmt, f->stream, sts, AVSEEK_FLAG_BACKWARD);
		logmsg("seek: timebase retry -> %d (t=%.2f ts=%lld)", r, t, (long long)sts);
	}
	if (r < 0) {
		AVStream *st = f->fmt->streams[f->stream];
		int64_t sts = (int64_t)(t / av_q2d(st->time_base));
		r = avformat_seek_file(f->fmt, f->stream, INT64_MIN, sts, INT64_MAX, 0);
		logmsg("seek: seek_file retry -> %d (t=%.2f)", r, t);
	}
	if (r < 0) {
		LeaveCriticalSection(&demux_lock);
		logmsg("seek: FAILED all strategies (pos=%lu t=%.3f, xmp-now=%.3f)", (unsigned long)pos, t,
			xmpfstatus ? xmpfstatus->GetTime() : -999.0);
		return -1;
	}
	logmsg("seek: ok pos=%lu t=%.3f (gran=%.3f, xmp-now=%.3f)", (unsigned long)pos, t,
			FF_GetGranularity(), xmpfstatus ? xmpfstatus->GetTime() : -999.0);

	avcodec_flush_buffers(f->dec);
	swr_init(f->swr); // drop any resampler delay
	f->outlen = f->outp = 0;
	f->eof = f->sent_eof = FALSE;
	f->demux_eof = FALSE;
	for (int i = 0; i < 8; i++) if (f->pend_a[i]) av_packet_free(&f->pend_a[i]); // pre-seek packets are stale
	f->pend_n = 0;

	// Clear the stale video queue + flush the video decoder (safe under
	// demux_lock: video_flush only takes video.lock, the established order).
	video_flush();
	{
		EnterCriticalSection(&video.lock);
		video.seeking = TRUE; // show black until the catch-up reaches the target
		LeaveCriticalSection(&video.lock);
	}

	// av_seek_frame lands on the keyframe <= t, which can be seconds (large
	// GOP) to tens of seconds (m3u8) behind t. Starting from there would make
	// audio + video crawl forward in real time, the picture lagging the clock
	// by that gap (the "像掉帧又不像" judder). Instead the AUDIO thread
	// fast-forwards the demuxer to t on its next call (f->ff_target - deferred
	// so a network download never blocks this UI thread): it reads + skips
	// audio packets, enqueues video packets so the render thread decodes +
	// drops them up to t quickly, and both streams then start AT the target.
	f->ff_target = t;
	LeaveCriticalSection(&demux_lock);
	video_clock_capture(t);
	f->pos = t;
	return t;
}

// ------------------------------------------------------------------ exported

extern "C" __declspec(dllexport) XMPIN *WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
	static XMPIN xmpin = {
		XMPIN_FLAG_CANSTREAM | XMPIN_FLAG_MULTIEXT | XMPIN_FLAG_NETSEEK | XMPIN_FLAG_OWNFILE,
		"FFmpeg (audio+video)",
		"FFmpeg audio\0"
			"mp3/mp2/mp1/mpa/m4a/m4b/m4r/aac/ac3/eac3/dts/dtshd/hra/mlp/truehd/thd/"
			"flac/ogg/oga/opus/spx/wma/wav/wave/w64/wv/ape/tta/tak/ofr/ofs/mpc/mp+/"
			"mpp/vqf/8svx/svx/amr/awb/gsm/gsm610/aa3/oma/at3/at9/ra/mka/caf/snd/au/aif/aiff/"
			"aifc/voc/iff/vox/ircam/amb/dsf/dff/xwb/aax/aa/ac4/aptx\0"
		"FFmpeg video (audio + video window)\0"
			"mkv/mp4/avi/mov/qt/wmv/asf/flv/f4v/webm/ogv/ts/m2ts/mts/m2t/mpg/mpeg/mpe/"
			"mpv/m2v/vob/ifo/3gp/3g2/3gpp/3gp2/divx/xvid/rm/rmvb/rv/m4v/tp/trp/dvr-ms/"
			"wtv/mxf/nuv/nsv/nut/roq/swf/fla/fli/flc/bik/bk2/sm2/r3d/yop/mtv/m2v\0"
		"\0",
		FF_About,
		NULL,             // no config
		FF_CheckFile,
		FF_GetFileInfo,
		FF_Open,
		FF_Close,
		NULL,
		FF_SetFormat,
		FF_GetTags,
		FF_GetInfoText,
		FF_GetGeneralInfo,
		FF_GetMessage,
		FF_SetPosition,
		FF_GetGranularity,
		NULL,             // GetBuffering (XMPFILE I/O)
		FF_Process,
		NULL,             // WriteFile
		NULL,             // GetSamples
		NULL,             // GetSubSongs
		NULL,
		NULL,             // GetDownloaded
		"FFmpeg video",   // visname (B2: video shows in XMPlay's vis area)
		FF_VisOpen,
		FF_VisClose,
		FF_VisSize,
		FF_VisRender,
		FF_VisRenderDC,
		NULL,             // VisButton (fullscreen is the vis area's own double-click)
		NULL,             // VisConfig ("video mode" toggle - add later)
	};

	if (face != XMPIN_FACE) {
		static int shownerror = 0;
		if (face < XMPIN_FACE && !shownerror) {
			MessageBox(0, PLUGIN_NAME " requires XMPlay 3.8 or above.", 0, MB_ICONEXCLAMATION);
			shownerror = 1;
		}
		return NULL;
	}
	xmpfin    = (XMPFUNC_IN*)faceproc(XMPFUNC_IN_FACE);
	xmpfmisc  = (XMPFUNC_MISC*)faceproc(XMPFUNC_MISC_FACE);
	xmpffile  = (XMPFUNC_FILE*)faceproc(XMPFUNC_FILE_FACE);
	xmpftext  = (XMPFUNC_TEXT*)faceproc(XMPFUNC_TEXT_FACE);
	xmpfstatus = (XMPFUNC_STATUS*)faceproc(XMPFUNC_STATUS_FACE);
	xmpver = xmpfmisc->GetVersion();
	InitializeCriticalSection(&video.lock);
	InitializeCriticalSection(&demux_lock);
	video.vframe = av_frame_alloc();
	av_log_set_callback(ff_log_cb);
	{
		void *it = NULL;
		const char *pn;
		char plist[400] = "";
		while ((pn = avio_enum_protocols(&it, 0))) {
			if (strlen(plist) + strlen(pn) + 2 < sizeof(plist)) {
				strcat(plist, pn);
				strcat(plist, " ");
			}
		}
		logmsg("=== plugin loaded: %s %s | flags=0x%lx | FFmpeg %s | protocols: %s",
			PLUGIN_NAME, PLUGIN_VER, (unsigned long)(XMPIN_FLAG_CANSTREAM | XMPIN_FLAG_MULTIEXT | XMPIN_FLAG_NETSEEK | XMPIN_FLAG_OWNFILE),
			av_version_info(), plist);
	}
	return &xmpin;
}

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hDLL, DWORD reason, LPVOID reserved)
{
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hDLL);
		break;
	}
	return TRUE;
}
#endif
