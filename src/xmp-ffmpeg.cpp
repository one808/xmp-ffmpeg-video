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
#include <ctype.h>

#include <winsock2.h>
#include <windows.h>
#include <wchar.h>
#include "xmpin.h"

#ifndef CP_936 // GBK / Chinese Simplified code page (XP-safe, from winnls.h)
#define CP_936 936
#endif

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
static XMPFUNC_REGISTRY *xmpfreg;
static DWORD xmpver;
static HINSTANCE dllinst;           // our DLL instance (vis config dialog resource)
static DWORD vis_colors[3];        // skin colors from FF_VisOpen (bg,text1,text2)

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

// ================================================================ lyrics layer
// LRC lyrics for AUDIO-ONLY tracks, shown in XMPlay's vis area (video tracks
// keep showing the picture). Sources, in order: a sidecar .lrc next to the
// file, embedded lyrics tags in the container, and (last resort) a network
// lyrics service. The network fetch runs on its own Winsock thread so it never
// blocks the audio path; results are committed only if the track generation
// still matches (a close/switch discards in-flight work).

// ------------------------------------------------------------------ settings
// Persisted through XMPlay's registry face under section "FFmpeg video".
#define LRC_REGSEC "FFmpeg video"
struct LRC_CFG {
	char service[256];  // lyrics service base URL
	int  use_service;   // download from the service when no local lyrics
	int  save_sidecar;  // save downloaded lyrics as a .lrc next to the track
	DWORD cur_color;    // 0xRRGGBB, colour of the active line
	char font_name[64];
	int  font_size;     // pixel height
	int  offset_tenths; // user time offset, tenths of a second (+ = lyrics lag)
};
static LRC_CFG lrc_cfg;
static void bcpy(char *dst, size_t dstn, const char *src); // forward (defn below)

static void lrc_cfg_defaults()
{
	static const LRC_CFG d = { "http://lrc.8od.top:88", 1, 1, 0x00ff88, "SimSun", 18, 0 };
	lrc_cfg = d;
}
static void lrc_cfg_load()
{
	lrc_cfg_defaults();
	if (!xmpfreg) return;
	{ char buf[256]; if (xmpfreg->GetString(LRC_REGSEC, "ServiceURL", buf, sizeof buf)) bcpy(lrc_cfg.service, sizeof lrc_cfg.service, buf); }
	int v;
	if (xmpfreg->GetInt(LRC_REGSEC, "UseService", &v))  lrc_cfg.use_service  = v ? 1 : 0;
	if (xmpfreg->GetInt(LRC_REGSEC, "SaveSidecar", &v)) lrc_cfg.save_sidecar = v ? 1 : 0;
	if (xmpfreg->GetInt(LRC_REGSEC, "CurColor", &v))    lrc_cfg.cur_color    = (DWORD)v;
	{ char buf[64]; if (xmpfreg->GetString(LRC_REGSEC, "FontName", buf, sizeof buf)) bcpy(lrc_cfg.font_name, sizeof lrc_cfg.font_name, buf); }
	if (xmpfreg->GetInt(LRC_REGSEC, "FontSize", &v) && v >= 8 && v <= 72) lrc_cfg.font_size = v;
	if (xmpfreg->GetInt(LRC_REGSEC, "OffsetTenths", &v) && v >= -600 && v <= 600) lrc_cfg.offset_tenths = v;
}
static void lrc_cfg_save()
{
	if (!xmpfreg) return;
	xmpfreg->SetString(LRC_REGSEC, "ServiceURL", lrc_cfg.service);
	{ int v = lrc_cfg.use_service  ? 1 : 0; xmpfreg->SetInt(LRC_REGSEC, "UseService", &v); }
	{ int v = lrc_cfg.save_sidecar ? 1 : 0; xmpfreg->SetInt(LRC_REGSEC, "SaveSidecar", &v); }
	{ int v = (int)lrc_cfg.cur_color;         xmpfreg->SetInt(LRC_REGSEC, "CurColor", &v); }
	xmpfreg->SetString(LRC_REGSEC, "FontName", lrc_cfg.font_name);
	xmpfreg->SetInt(LRC_REGSEC, "FontSize", &lrc_cfg.font_size);
	{ int v = lrc_cfg.offset_tenths; xmpfreg->SetInt(LRC_REGSEC, "OffsetTenths", &v); }
}

// -------------------------------------------------------------- lyrics store
typedef struct { double t; char *text; } LrcLine; // text = UTF-8, heap

struct LYRIC {
	CRITICAL_SECTION lock;
	LrcLine *lines;
	int nlines;
	int state;          // 0 = none, 1 = fetching, 2 = ready
	volatile DWORD gen; // bumped on every open/close; stale fetches are dropped
};
static LYRIC lyric;
static CRITICAL_SECTION lyric_thread_lock; // guards spawn/handle
static HANDLE lyric_fetch_handle;

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = (char*)malloc(n);
	if (p) memcpy(p, s, n);
	return p;
}
// bounded copy (MinGW lacks MSVC's strcpy_s); dst must be NUL-terminated
static void bcpy(char *dst, size_t dstn, const char *src)
{
	if (dstn == 0) return;
	strncpy(dst, src, dstn - 1);
	dst[dstn - 1] = 0;
}
static void lrc_free_lines(LrcLine *lines, int n)
{
	for (int i = 0; i < n; i++) free(lines[i].text);
	free(lines);
}

// ------------------------------------------------------------- LRC text parse
// parse one leading "[mm:ss.xx]" timestamp; s must point at '['. Returns the
// number of chars consumed (including the closing ']'), 0 if not a timestamp.
static int lrc_ts_at(const char *s, double *t)
{
	if (s[0] != '[') return 0;
	const char *end = s + 1;
	while (*end && *end != ']') end++;
	if (*end != ']') return 0;
	const char *p = s + 1;
	int mm = 0, ss = 0, frac = 0, nm = 0, ns = 0, nf = 0;
	while (*p >= '0' && *p <= '9' && nm < 3) { mm = mm * 10 + (*p - '0'); p++; nm++; }
	if (!nm || *p != ':') return 0;
	p++;
	while (*p >= '0' && *p <= '9' && ns < 2) { ss = ss * 10 + (*p - '0'); p++; ns++; }
	if (!ns) return 0;
	if (*p == '.' || *p == ':') {
		p++;
		while (*p >= '0' && *p <= '9' && nf < 3) { frac = frac * 10 + (*p - '0'); p++; nf++; }
	}
	double fracsec = 0;
	if (nf == 1) fracsec = frac / 10.0;
	else if (nf == 2) fracsec = frac / 100.0;
	else if (nf == 3) fracsec = frac / 1000.0;
	*t = mm * 60.0 + ss + fracsec;
	return (int)(end - s) + 1;
}
static int looks_like_lrc(const char *v)
{
	const char *p = v;
	while ((p = strchr(p, '['))) {
		if (p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9' && p[3] == ':') return 1;
		p++;
	}
	return 0;
}
// Parse LRC text into a heap line array. Returns the line count (0 = none).
static int lrc_parse(const char *text, LrcLine **out)
{
	LrcLine *lines = 0;
	int n = 0, cap = 0;
	const char *s = text;
	while (*s) {
		const char *nl = strchr(s, '\n');
		int len = nl ? (int)(nl - s) : (int)strlen(s);
		char *line = (char*)malloc(len + 1);
		if (line) {
			memcpy(line, s, len);
			line[len] = 0;
			if (len && line[len - 1] == '\r') line[len - 1] = 0;
			// collect the leading [mm:ss.xx] tags (a line may carry several)
			const char *p = line;
			double ts[16];
			int nts = 0;
			while (*p == '[' && nts < 16) {
				double t;
				int c = lrc_ts_at(p, &t);
				if (!c) break;
				ts[nts++] = t;
				p += c;
			}
			if (nts) {
				// the text is what follows the tags; strip inline <mm:ss.xx>
				char *w = (char*)malloc(strlen(p) + 1);
				if (w) {
					char *o = w;
					while (*p) {
						if (*p == '<') {
							const char *g = strchr(p, '>');
							if (g) { p = g + 1; continue; }
						}
						*o++ = *p++;
					}
					*o = 0;
					o = w; // the copy loop advanced o to the end; reset to the start
					while (*o == ' ' || *o == '\t') o++;        // ltrim
					char *b = o + strlen(o);
					while (b > o && (b[-1] == ' ' || b[-1] == '\t')) b--; // rtrim
					*b = 0;
					if (*o) {
						for (int k = 0; k < nts; k++) {
							if (n == cap) {
								cap = cap ? cap * 2 : 16;
								lines = (LrcLine*)realloc(lines, cap * sizeof(LrcLine));
								if (!lines) { free(w); lrc_free_lines(lines, n); *out = 0; return 0; }
							}
							lines[n].t = ts[k];
							lines[n].text = xstrdup(o);
							if (!lines[n].text) { lrc_free_lines(lines, n); *out = 0; return 0; }
							n++;
						}
					}
					free(w);
				}
			}
			free(line);
		}
		if (!nl) break;
		s = nl + 1;
	}
	// a multi-verse (e.g. 酷我-compressed) LRC lists lines out of order and
	// splits one source line into several entries. The renderer assumes the
	// array is ascending in time (current line = last whose time passed, and
	// the scroll window reads consecutive entries), so sort here. Stable
	// insertion sort keeps equal-time entries in source order. The array is
	// small (a few hundred at most) so O(n^2) is fine.
	for (int i = 1; i < n; i++) {
		LrcLine key = lines[i];
		int j = i - 1;
		while (j >= 0 && lines[j].t > key.t) {
			lines[j + 1] = lines[j];
			j--;
		}
		lines[j + 1] = key;
	}
	*out = lines;
	return n;
}

// --------------------------------------------------------- UTF-8 <-> UTF-16-LE
// the service encodes artist/title as UTF-16-LE bytes hex-doubled
static char *utf8_to_hex16le(const char *utf8)
{
	if (!utf8 || !*utf8) return xstrdup("");
	int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, 0, 0);
	if (wn < 1) return xstrdup("");
	wchar_t *w = (wchar_t*)malloc(wn * sizeof(wchar_t));
	if (!w) return 0;
	MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, wn);
	// on x86 wchar_t is 16-bit little-endian; hex the raw bytes (drop the
	// trailing NUL wide char). Cap the number of chars so the hex (and the
	// URLs built from it) stay bounded - artist/title are short in practice.
	int chars = wn - 1;
	if (chars > 64) chars = 64;
	char *h = (char*)malloc(chars * 4 + 1);
	if (!h) { free(w); return 0; }
	for (int i = 0; i < chars; i++) {
		unsigned short u = (unsigned short)w[i];
		sprintf(h + i * 4, "%02x%02x", (unsigned)(u & 0xff), (unsigned)((u >> 8) & 0xff));
	}
	h[chars * 4] = 0;
	free(w);
	return h;
}

// ------------------------------------------------------------- HTTP GET (WS)
static volatile LONG ws_init_done = 0;
static void ws_ensure(void)
{
	if (InterlockedCompareExchange(&ws_init_done, 1, 0) == 0) {
		WSADATA d;
		WSAStartup(MAKEWORD(2, 2), &d);
	}
}
static int ci_contains(const char *h, const char *needle)
{
	size_t nl = strlen(needle);
	for (const char *p = h; *p; p++) {
		int ok = 1;
		for (size_t i = 0; i < nl; i++)
			if (toupper((unsigned char)p[i]) != toupper((unsigned char)needle[i])) { ok = 0; break; }
		if (ok) return 1;
	}
	return 0;
}
// fetch a URL with plain Winsock. Returns a malloc'd body (NUL-terminated,
// headers stripped, chunked decoded) or 0 on any failure. Bounded work, 8s
// socket timeouts, ~2MB cap.
static char *http_get(const char *url)
{
	ws_ensure();
	char host[256], path[1024];
	int port = 0;
	const char *p = url;
	if (strncmp(p, "http://", 7) == 0) p += 7;
	else if (strncmp(p, "https://", 8) == 0) return 0; // no TLS on XP
	{
		const char *h = p;
		char *ho = host;
		int hi = 0;
		while (*h && *h != '/' && *h != ':' && hi < (int)sizeof host - 1) ho[hi++] = *h++;
		ho[hi] = 0;
		port = 80;
		if (*h == ':') {
			h++;
			port = atoi(h);
			while (*h && *h != '/') h++;
		}
		bcpy(path, sizeof path, h);
		if (!host[0] || !port || !path[0]) return 0;
	}
	// resolve + connect (gethostbyname: universally available on XP/mingw;
	// getaddrinfo is not declared by this toolchain's winsock2.h)
	struct hostent *he = gethostbyname(host);
	if (!he || !he->h_addr_list[0]) return 0;
	SOCKET s = INVALID_SOCKET;
	for (int i = 0; he->h_addr_list[i]; i++) {
		struct sockaddr_in sa;
		memset(&sa, 0, sizeof sa);
		sa.sin_family = AF_INET;
		sa.sin_port = htons((u_short)port);
		memcpy(&sa.sin_addr, he->h_addr_list[i], he->h_length);
		s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET) continue;
		if (connect(s, (struct sockaddr*)&sa, sizeof sa) == 0) break;
		closesocket(s);
		s = INVALID_SOCKET;
	}
	if (s == INVALID_SOCKET) return 0;
	{
		DWORD to = 8000;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof to);
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&to, sizeof to);
	}
	char req[1400];
	snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"
		"User-Agent: xmp-ffmpeg\r\nAccept: */*\r\n\r\n", path, host, port);
	if (send(s, req, (int)strlen(req), 0) < 0) { closesocket(s); return 0; }
	char *body = 0;
	DWORD cap = 0, len = 0;
	char tmp[4096];
	while (1) {
		int r = recv(s, tmp, sizeof tmp, 0);
		if (r <= 0) break;
		if (len + (DWORD)r + 1 > cap) {
			cap = cap ? cap * 2 : 4096;
			while (cap < len + (DWORD)r + 1) cap *= 2;
			body = (char*)realloc(body, cap);
			if (!body) break;
		}
		memcpy(body + len, tmp, r);
		len += r;
		body[len] = 0;
		if (len > 2048 * 1024) break;
	}
	closesocket(s);
	if (!body) return 0;
	// strip the HTTP header block so the caller gets just the body
	char *hs = strstr(body, "\r\n\r\n");
	char *bstart = hs ? hs + 4 : body;
	if (ci_contains(body, "Transfer-Encoding") && ci_contains(body, "chunked")) {
		char *src = bstart, *dst = body;
		while (*src) {
			char *eol = strstr(src, "\r\n");
			if (!eol) break;
			int szd = (int)(eol - src);
			if (szd > 23) szd = 23;
			char szbuf[32];
			memcpy(szbuf, src, szd);
			szbuf[szd] = 0;
			unsigned cs = (unsigned)strtoul(szbuf, 0, 16);
			src = eol + 2;
			if (cs == 0) break;
			memcpy(dst, src, cs);
			dst += cs;
			src += cs;
			if (src[0] == '\r' && src[1] == '\n') src += 2;
		}
		*dst = 0;
	} else {
		// Content-Length response: move the body to the front, drop the headers
		size_t blen = strlen(bstart);
		memmove(body, bstart, blen + 1);
	}
	return body;
}

// -------------------------------------------------------- sidecar .lrc helper
// LRC files on disk are often GBK (Chinese Windows) or UTF-16; normalize to
// UTF-8 so the parser + GDI wide conversion work on one encoding.
static char *copy_bytes(const unsigned char *src, size_t n)
{
	char *p = (char*)malloc(n + 1);
	if (p) { memcpy(p, src, n); p[n] = 0; }
	return p;
}
static int looks_utf8(const unsigned char *b, size_t n)
{
	size_t i = 0;
	while (i < n) {
		unsigned char c = b[i];
		if (c < 0x80) { i++; continue; }
		int extra;
		if (c >= 0xC2 && c <= 0xDF) extra = 1;
		else if (c >= 0xE0 && c <= 0xEF) extra = 2;
		else if (c >= 0xF0 && c <= 0xF4) extra = 3;
		else return 0;
		if (i + (size_t)extra >= n) return 0;
		for (int k = 1; k <= extra; k++) if ((b[i + k] & 0xC0) != 0x80) return 0;
		i += 1 + extra;
	}
	return 1;
}
static char *utf16_to_utf8(const unsigned char *bytes, size_t bytelen, int is_be)
{
	wchar_t *w = (wchar_t*)malloc((bytelen / 2 + 1) * sizeof(wchar_t));
	if (!w) return 0;
	for (size_t i = 0; i + 1 < bytelen; i += 2)
		w[i / 2] = is_be ? ((wchar_t)bytes[i] << 8) | bytes[i + 1]
		: ((wchar_t)bytes[i + 1] << 8) | bytes[i];
	w[bytelen / 2] = 0;
	int outc = WideCharToMultiByte(CP_UTF8, 0, w, (int)(bytelen / 2), 0, 0, 0, 0);
	char *out = 0;
	if (outc > 0) {
		out = (char*)malloc(outc + 1);
		if (out) WideCharToMultiByte(CP_UTF8, 0, w, (int)(bytelen / 2), out, outc, 0, 0);
	}
	free(w);
	if (out && out[0]) return out;
	free(out);
	return 0;
}
static char *cp936_to_utf8(const unsigned char *bytes, size_t bytelen)
{
	int wlen = MultiByteToWideChar(CP_936, 0, (const char*)bytes, (int)bytelen, 0, 0);
	if (wlen < 0) return 0;
	wchar_t *w = (wchar_t*)malloc((wlen + 1) * sizeof(wchar_t));
	if (!w) return 0;
	MultiByteToWideChar(CP_936, 0, (const char*)bytes, (int)bytelen, w, wlen);
	int outc = WideCharToMultiByte(CP_UTF8, 0, w, wlen, 0, 0, 0, 0);
	char *out = 0;
	if (outc > 0) {
		out = (char*)malloc(outc + 1);
		if (out) WideCharToMultiByte(CP_UTF8, 0, w, wlen, out, outc, 0, 0);
	}
	free(w);
	return out;
}
// normalize arbitrary LRC bytes to a heap UTF-8 string (or 0 on failure)
static char *lrc_to_utf8(const unsigned char *raw, size_t len)
{
	if (len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF)
		return copy_bytes(raw + 3, len - 3);                       // UTF-8 BOM
	if (len >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
		return utf16_to_utf8(raw + 2, len - 2, 0);                 // UTF-16LE BOM
	if (len >= 2 && raw[0] == 0xFE && raw[1] == 0xFF)
		return utf16_to_utf8(raw + 2, len - 2, 1);                 // UTF-16BE BOM
	{ // no BOM: mostly-NUL -> UTF-16LE
		int nul = 0, nonnul = 0;
		for (size_t i = 0; i < len; i++) (raw[i] == 0) ? nul++ : nonnul++;
		if (nul > 0 && nul >= nonnul) return utf16_to_utf8(raw, len, 0);
	}
	if (looks_utf8(raw, len)) return copy_bytes(raw, len);          // plain UTF-8
	return cp936_to_utf8(raw, len);                                  // else GBK/CP936
}
static wchar_t *utf8_path_to_wide(const char *s)
{
	int wn = MultiByteToWideChar(CP_UTF8, 0, s, -1, 0, 0);
	if (wn < 1) return 0;
	wchar_t *w = (wchar_t*)malloc(wn * sizeof(wchar_t));
	if (!w) return 0;
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wn);
	return w;
}
// build "<dir>/<base>.lrc" for a local file path; returns a heap wide path or 0.
static wchar_t *sidecar_wide_path(const char *filename_utf8)
{
	wchar_t *wf = utf8_path_to_wide(filename_utf8);
	if (!wf) return 0;
	size_t n = wcslen(wf);
	wchar_t *side = (wchar_t*)malloc((n + 8) * sizeof(wchar_t));
	if (!side) { free(wf); return 0; }
	wcscpy(side, wf);
	// strip the extension: find the last '.' that sits after the last path sep
	wchar_t *base = side + n;
	while (base > side && *base != L'\\' && *base != L'/') base--;
	wchar_t *dot = 0;
	for (wchar_t *q = base + 1; *q; q++) if (*q == L'.') dot = q;
	if (dot) *dot = 0; // no extension -> nothing stripped, name kept as-is
	wcscat(side, L".lrc");
	free(wf);
	return side;
}
static char *read_sidecar(const char *filename_utf8)
{
	wchar_t *wp = sidecar_wide_path(filename_utf8);
	if (!wp) return 0;
	FILE *f = _wfopen(wp, L"rb");
	free(wp);
	if (!f) return 0;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
	long sz = ftell(f);
	rewind(f);
	if (sz < 0) { fclose(f); return 0; }
	unsigned char *raw = (unsigned char*)malloc((size_t)sz);
	if (!raw) { fclose(f); return 0; }
	size_t rd = fread(raw, 1, (size_t)sz, f);
	fclose(f);
	char *utf8 = lrc_to_utf8(raw, rd);
	free(raw);
	if (!utf8 || !looks_like_lrc(utf8)) { free(utf8); return 0; }
	return utf8;
}
static BOOL save_sidecar(const char *filename_utf8, const char *lrc_text)
{
	wchar_t *wp = sidecar_wide_path(filename_utf8);
	if (!wp) return FALSE;
	FILE *f = _wfopen(wp, L"wb");
	free(wp);
	if (!f) return FALSE;
	size_t wr = fwrite(lrc_text, 1, strlen(lrc_text), f);
	fclose(f);
	return wr == strlen(lrc_text);
}
static char *read_embedded(FFMPEG *f)
{
	const char *keys[] = { "LYRICS", "lyrics", "unsynced_lyrics", "SYNCLRC", "comment" };
	AVDictionary *md[2] = { f->fmt->metadata, f->fmt->streams[f->stream]->metadata };
	for (int i = 0; i < 2; i++) {
		if (!md[i]) continue;
		for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
			AVDictionaryEntry *e = av_dict_get(md[i], keys[k], 0, 0);
			if (e && e->value && e->value[0] && looks_like_lrc(e->value)) return xstrdup(e->value);
		}
	}
	return 0;
}

// ------------------------------------------------------------- fetch thread
// tiny XML helpers: the search endpoint returns <lrc id="N" artist=".."
// title=".." type=".."/> lines; we score candidates against the requested
// artist/title so a DJ/cover version doesn't win over the right song.
static int xml_attr(const char *region, const char *key, char *out, int outc)
{
	char pat[64];
	snprintf(pat, sizeof pat, "%s=\"", key);
	const char *p = strstr(region, pat);
	if (!p) return 0;
	p += strlen(pat);
	const char *end = strchr(p, '"');
	int n = end ? (int)(end - p) : (int)strlen(p);
	if (n >= outc) n = outc - 1;
	memcpy(out, p, n);
	out[n] = 0;
	return n > 0;
}
static int ci_eq_str(const char *a, const char *b)
{
	if (!a) a = "";
	if (!b) b = "";
	size_t la = strlen(a), lb = strlen(b);
	if (la != lb) return 0;
	for (size_t i = 0; i < la; i++)
		if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i])) return 0;
	return 1;
}
static int ci_contains_str(const char *hay, const char *needle)
{
	if (!hay || !needle || !*needle) return 0;
	size_t nl = strlen(needle);
	const char *p = hay;
	while (*p) {
		if (strlen(p) >= nl) {
			int ok = 1;
			for (size_t i = 0; i < nl; i++)
				if (toupper((unsigned char)p[i]) != toupper((unsigned char)needle[i])) { ok = 0; break; }
			if (ok) return 1;
		}
		p++;
	}
	return 0;
}

typedef struct {
	char service[256];
	char artist[512];
	char title[512];
	char filename[1024];
	int use_service;
	int save_sidecar;
	DWORD gen;
} LyricFetchCtx;

static DWORD WINAPI lyric_fetch_thread(LPVOID arg)
{
	LyricFetchCtx *c = (LyricFetchCtx*)arg;
	DWORD gen = c->gen;
	char *lrc_text = 0;
	// search: artist+title -> best candidate id (need at least one to search by)
	if (c->artist[0] || c->title[0]) {
		char *ah = utf8_to_hex16le(c->artist);
		char *th = utf8_to_hex16le(c->title);
		if (ah && th) {
			char url[4096];
			snprintf(url, sizeof url, "%s/?sh?Artist=%s&Title=%s", c->service, ah, th);
			logmsg("lrc: search %s", url);
			char *xml = http_get(url);
			if (!xml) logmsg("lrc: search http failed (url=%s)", url);
			if (xml) {
				// score every <lrc> candidate against the requested title/artist
				int best = -1;
				double best_score = -1;
				const char *s = xml;
				while ((s = strstr(s, "<lrc "))) {
					const char *tagend = strchr(s, '>');
					if (!tagend) break;
					char region[1024];
					int rlen = (int)(tagend - s);
					if (rlen >= (int)sizeof region) rlen = sizeof region - 1;
					memcpy(region, s, rlen);
					region[rlen] = 0;
					char idbuf[16], abuf[256], tbuf[256];
					int id = 0;
					if (xml_attr(region, "id", idbuf, sizeof idbuf)) id = atoi(idbuf);
					xml_attr(region, "artist", abuf, sizeof abuf);
					xml_attr(region, "title", tbuf, sizeof tbuf);
					if (id > 0) {
						double score = 0;
						if (ci_eq_str(tbuf, c->title)) score += 10;
						else if (ci_contains_str(tbuf, c->title) ||
						         (c->title[0] && ci_contains_str(c->title, tbuf))) score += 3;
						if (c->artist[0] &&
							(ci_contains_str(abuf, c->artist) || ci_contains_str(c->artist, abuf)))
							score += 5;
						if (score > best_score) { best_score = score; best = id; }
					}
					s = tagend + 1;
				}
				if (best > 0) {
					char dlurl[600];
					snprintf(dlurl, sizeof dlurl, "%s/?dl?Id=%d&Code=1", c->service, best);
					logmsg("lrc: download id=%d (score=%.0f)", best, best_score);
					char *body = http_get(dlurl);
					if (body && looks_like_lrc(body)) {
						lrc_text = body;
						{ // diagnostic: length + escaped head so field issues are visible
							char head[97];
							unsigned char *b = (unsigned char*)body;
							int hl = (int)strlen(body);
							if (hl > 48) hl = 48;
							for (int i = 0; i < hl; i++) head[i * 2] = (b[i] >= 32 && b[i] < 127) ? b[i] : '.';
							head[hl * 2] = 0;
							logmsg("lrc: got %lu bytes, head=\"%s\"", (unsigned long)strlen(body), head);
						}
					}
					else { logmsg("lrc: download failed or not LRC (id=%d)", best); free(body); }
				} else {
					logmsg("lrc: no <lrc> candidate in search result");
				}
				free(xml);
			}
			free(ah);
			free(th);
		}
	} else {
		logmsg("lrc: service skipped: no artist/title");
	}
	EnterCriticalSection(&lyric.lock);
	BOOL fresh = (lyric.gen == gen);
	if (fresh && lrc_text) {
		lrc_free_lines(lyric.lines, lyric.nlines);
		lyric.lines = 0;
		lyric.nlines = 0;
		lyric.nlines = lrc_parse(lrc_text, &lyric.lines);
		lyric.state = lyric.nlines > 0 ? 2 : 0;
		if (lyric.nlines > 0 && c->save_sidecar && !strstr(c->filename, "://")) {
			if (save_sidecar(c->filename, lrc_text))
				logmsg("lrc: sidecar saved next to %s", c->filename);
			else
				logmsg("lrc: sidecar save FAILED for %s", c->filename);
		}
		logmsg("lrc: ready lines=%d state=%d", lyric.nlines, lyric.state);
	} else {
		logmsg("lrc: fetch stale, dropped");
	}
	LeaveCriticalSection(&lyric.lock);
	free(lrc_text);
	free(c);
	// keep the handle valid until the next spawn reuses/closes it
	return 0;
}

// Kick off lyric loading for an audio-only track. Sidecar + embedded are read
// synchronously (fast, local); only the service download goes to a thread.
static void lyric_start(FFMPEG *f, const char *filename, const char *artist, const char *title, BOOL is_local)
{
	EnterCriticalSection(&lyric.lock);
	lyric.gen++;
	DWORD gen = lyric.gen;
	lrc_free_lines(lyric.lines, lyric.nlines);
	lyric.lines = 0;
	lyric.nlines = 0;
	lyric.state = 0;
	LeaveCriticalSection(&lyric.lock);

	char *text = 0;
	const char *src = 0;
	if (is_local) { text = read_sidecar(filename); src = "sidecar"; }
	if (!text) { text = read_embedded(f); src = "embedded"; }
	if (text) {
		BOOL ok = FALSE;
		EnterCriticalSection(&lyric.lock);
		if (lyric.gen == gen) {
			lyric.nlines = lrc_parse(text, &lyric.lines);
			lyric.state = lyric.nlines > 0 ? 2 : 0;
			ok = lyric.nlines > 0;
			logmsg("lrc: %s lyrics lines=%d", src, lyric.nlines);
		}
		LeaveCriticalSection(&lyric.lock);
		free(text);
		if (ok) return;
		// unparsable local lyrics: fall through to the service
	}
	if (!lrc_cfg.use_service || !lrc_cfg.service[0]) {
		logmsg("lrc: no local lyrics and service off -> none");
		return;
	}
	logmsg("lrc: no local lyrics, fetching from %s (artist=\"%s\" title=\"%s\")",
		lrc_cfg.service, artist ? artist : "", title ? title : "");
	// spawn the network fetch (only one in flight; a stale one drops its result)
	EnterCriticalSection(&lyric_thread_lock);
	LyricFetchCtx *c = (LyricFetchCtx*)calloc(1, sizeof(LyricFetchCtx));
	if (c) {
		bcpy(c->service, sizeof c->service, lrc_cfg.service);
		if (artist) bcpy(c->artist, sizeof c->artist, artist);
		if (title) bcpy(c->title, sizeof c->title, title);
		if (filename) bcpy(c->filename, sizeof c->filename, filename);
		c->use_service = lrc_cfg.use_service;
		c->save_sidecar = lrc_cfg.save_sidecar && is_local ? 1 : 0;
		c->gen = gen;
		EnterCriticalSection(&lyric.lock);
		lyric.state = 1; // fetching
		LeaveCriticalSection(&lyric.lock);
		HANDLE h = CreateThread(0, 0, lyric_fetch_thread, c, 0, 0);
		if (h) {
			HANDLE old = lyric_fetch_handle;
			lyric_fetch_handle = h;
			// close the previous handle only after we've handed off; the thread
			// object outlives the handle, so closing the old one is safe
			if (old) CloseHandle(old);
		} else {
			free(c);
			EnterCriticalSection(&lyric.lock);
			lyric.state = 0;
			LeaveCriticalSection(&lyric.lock);
		}
	}
	LeaveCriticalSection(&lyric_thread_lock);
}

// called on Close: invalidate any in-flight/finished lyrics for this track
static void lyric_invalidate()
{
	EnterCriticalSection(&lyric.lock);
	lyric.gen++;
	lrc_free_lines(lyric.lines, lyric.nlines);
	lyric.lines = 0;
	lyric.nlines = 0;
	lyric.state = 0;
	LeaveCriticalSection(&lyric.lock);
}

// --------------------------------------------------------------- vis renderer
static HFONT lrc_font;
static char lrc_font_cache[64];
static int lrc_font_cache_size = -1;

static int lyric_render(HDC dc, int W, int H)
{
	// returns 1 if lyrics were drawn, 0 otherwise (caller falls back to black)
	// effective time = playback clock - user offset (+offset = lyrics lag)
	double t = video_clock_now() - (double)lrc_cfg.offset_tenths * 0.1;
	int lh = lrc_cfg.font_size + 6; // line height in px
	int rows = H / lh;
	if (rows < 3) rows = 3;
	if (rows > 12) rows = 12;
	int mid = rows / 2; // snapshot slot the current line lands in
	const char *texts[32];
	int n = 0, cur = -1, i0 = 0;
	{
		EnterCriticalSection(&lyric.lock);
		if (lyric.state == 2 && lyric.nlines > 0) {
			// current line = the last line whose time has passed. Full linear
			// scan: the list is small (at most a few hundred) and a capped
			// scan here froze the display at line 32.
			for (int i = 0; i < lyric.nlines; i++)
				if (t >= lyric.lines[i].t) cur = i;
			if (cur < 0) cur = 0;
			i0 = cur - mid; // place the current line mid-panel; older lines
			if (i0 < 0) i0 = 0; // scroll up out of the top, newer from below
			for (int i = i0; i < lyric.nlines && n < 32; i++) {
				texts[n] = lyric.lines[i].text;
				n++;
			}
		}
		LeaveCriticalSection(&lyric.lock);
	}
	if (n == 0) return 0;

	// rebuild the font if the config changed
	{
		BOOL changed = (lrc_font_cache_size != lrc_cfg.font_size);
		if (!changed) {
			int len = (int)strlen(lrc_cfg.font_name);
			if (len >= (int)sizeof lrc_font_cache) changed = 1;
			else if (len != (int)strlen(lrc_font_cache)) changed = 1;
			else for (int i = 0; i < len; i++) if (lrc_font_cache[i] != lrc_cfg.font_name[i]) { changed = 1; break; }
		}
		if (changed) {
			bcpy(lrc_font_cache, sizeof lrc_font_cache, lrc_cfg.font_name);
			lrc_font_cache_size = lrc_cfg.font_size;
			if (lrc_font) DeleteObject(lrc_font);
			wchar_t fn[64];
			MultiByteToWideChar(CP_UTF8, 0, lrc_cfg.font_name, -1, fn, 64);
			lrc_font = CreateFontW(-(lrc_cfg.font_size), 0, 0, 0, FW_NORMAL, FALSE, FALSE, 0,
				DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
				DEFAULT_PITCH | FF_DONTCARE, fn);
			if (!lrc_font) lrc_font = CreateFontW(-lrc_cfg.font_size, 0, 0, 0, FW_NORMAL, 0, 0, 0,
				DEFAULT_CHARSET, 0, 0, DEFAULT_QUALITY, DEFAULT_PITCH, L"Tahoma");
		}
	}

	int currow = cur - i0; // snapshot slot of the current line (0..mid)
	int bg = (int)vis_colors[0];
	int txt = (int)vis_colors[1];
	int R = (bg >> 16) & 0xff, G = (bg >> 8) & 0xff, B = bg & 0xff;
	int tr = (txt >> 16) & 0xff, tg = (txt >> 8) & 0xff, tb = txt & 0xff;
	int dr = R + (tr - R) / 2, dg = G + (tg - G) / 2, db = B + (tb - B) / 2; // dimmed
	HFONT oldf = (HFONT)SelectObject(dc, lrc_font);
	HPEN oldpen = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
	HGDIOBJ oldbr = SelectObject(dc, GetStockObject(NULL_BRUSH));
	for (int r = 0; r < rows; r++) {
		if (r >= n) continue; // texts[r] = lines[i0 + r]
		wchar_t wtext[512];
		MultiByteToWideChar(CP_UTF8, 0, texts[r], -1, wtext, 512);
		RECT rc = { 0, r * lh, W, r * lh + lh };
		COLORREF cr = (r == currow) ? (COLORREF)lrc_cfg.cur_color : RGB(dr, dg, db);
		SetTextColor(dc, cr);
		SetBkMode(dc, TRANSPARENT);
		DrawTextW(dc, wtext, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}
	SelectObject(dc, oldf);
	SelectObject(dc, oldpen);
	SelectObject(dc, oldbr);
	return 1;
}

// ---------------------------------------------------------------- vis config
static HWND lrc_confwin;

BOOL CALLBACK LrcConfigDialogProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	switch (m) {
	case WM_INITDIALOG:
		lrc_confwin = h;
		{
			POINT p;
			GetCursorPos(&p);
			SetWindowPos(h, 0, p.x - 160, p.y - 20, 0, 0,
				SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
		}
		SetDlgItemText(h, 201, lrc_cfg.service);
		SendDlgItemMessage(h, 202, BM_SETCHECK, lrc_cfg.use_service ? BST_CHECKED : BST_UNCHECKED, 0);
		SendDlgItemMessage(h, 203, BM_SETCHECK, lrc_cfg.save_sidecar ? BST_CHECKED : BST_UNCHECKED, 0);
		{ char b[32]; sprintf(b, "%06X", (unsigned)lrc_cfg.cur_color); SetDlgItemText(h, 204, b); }
		SetDlgItemText(h, 205, lrc_cfg.font_name);
		{ char b[16]; sprintf(b, "%d", lrc_cfg.font_size); SetDlgItemText(h, 206, b); }
		{ char b[16]; sprintf(b, "%d", lrc_cfg.offset_tenths); SetDlgItemText(h, 207, b); }
		return 1;
	case WM_COMMAND:
		switch (LOWORD(w)) {
		case IDOK:
		{
			char buf[256];
			GetDlgItemText(h, 201, buf, sizeof buf);
			bcpy(lrc_cfg.service, sizeof lrc_cfg.service, buf);
			lrc_cfg.use_service  = SendDlgItemMessage(h, 202, BM_GETCHECK, 0, 0) == BST_CHECKED;
			lrc_cfg.save_sidecar = SendDlgItemMessage(h, 203, BM_GETCHECK, 0, 0) == BST_CHECKED;
			{ char cb[32]; GetDlgItemText(h, 204, cb, sizeof cb);
			  if (strlen(cb) == 6) lrc_cfg.cur_color = (DWORD)strtoul(cb, 0, 16); }
			GetDlgItemText(h, 205, buf, sizeof buf);
			bcpy(lrc_cfg.font_name, sizeof lrc_cfg.font_name, buf);
			{ int sz = GetDlgItemInt(h, 206, NULL, TRUE); if (sz >= 8 && sz <= 72) lrc_cfg.font_size = sz; }
			{ int of = GetDlgItemInt(h, 207, NULL, TRUE); if (of >= -600 && of <= 600) lrc_cfg.offset_tenths = of; }
			lrc_cfg_save();
			EndDialog(h, 1);
			break;
		}
		case IDCANCEL:
			EndDialog(h, 0);
			break;
		}
		return 1;
	case WM_DESTROY:
		lrc_confwin = 0;
		break;
	}
	return 0;
}

static void WINAPI FF_VisConfig(HWND win)
{
	if (lrc_confwin) SetForegroundWindow(lrc_confwin);
	else DialogBox(dllinst, MAKEINTRESOURCE(1000), win, LrcConfigDialogProc);
}

// ------------------------------------------------------- XMPlay vis callbacks
// B2: the video shows in XMPlay's OWN visualisation area. XMPlay opens/closes/
// fullscreen-s it (VisOpen/VisClose + the area's own double-click); we just
// hand it the latest decoded frame via FF_VisRenderDC. No window of our own.

static BOOL WINAPI FF_VisOpen(DWORD colors[3])
{
	logmsg("vis: open");
	if (colors) { vis_colors[0] = colors[0]; vis_colors[1] = colors[1]; vis_colors[2] = colors[2]; }
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

	// no video stream on this track (audio-only): show the skin background plus
	// LRC lyrics when we have them (fetching happens on a background thread)
	if (!video.dib || video.dibw < 1 || !video.vdec) {
		HBRUSH bg = CreateSolidBrush((COLORREF)vis_colors[0]);
		FillRect(dc, &rc, bg);
		DeleteObject(bg);
		lyric_render(dc, size.cx, size.cy); // draws nothing until lyrics are ready
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
		"including the audio track of video files.\n\n"
		"Video renders into XMPlay's own visualisation area:\n"
		"pick \"FFmpeg video / lyrics\" in the vis menu; XMPlay's vis\n"
		"controls (open/close/fullscreen) drive the picture. Audio-only\n"
		"tracks show LRC lyrics there instead (sidecar .lrc, embedded\n"
		"tags, or the configured lyrics service).\n\n"
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
	if (current->vstream < 0) {
		// audio-only track: load LRC lyrics (sidecar -> embedded tags -> service)
		char artist[512] = "", title[512] = "";
		AVDictionary *md[2] = { current->fmt->metadata,
			current->fmt->streams[current->stream]->metadata };
		for (int i = 0; i < 2 && !artist[0]; i++) {
			if (!md[i]) continue;
			AVDictionaryEntry *e = av_dict_get(md[i], "artist", 0, 0);
			if (e && e->value) bcpy(artist, sizeof artist, e->value);
		}
		for (int i = 0; i < 2 && !title[0]; i++) {
			if (!md[i]) continue;
			AVDictionaryEntry *e = av_dict_get(md[i], "title", 0, 0);
			if (e && e->value) bcpy(title, sizeof title, e->value);
		}
		BOOL is_local = !current->netfile && !(filename && strstr(filename, "://"));
		if (!title[0] && is_local && filename) {
			// untagged local file: derive a search title from the file name
			// ("Artist - Title.mp3" and "Title.mp3" both work with the service)
			const char *base = filename;
			for (const char *q = filename; *q; q++)
				if (*q == '\\' || *q == '/') base = q + 1;
			char tbuf[512];
			bcpy(tbuf, sizeof tbuf, base);
			char *dot = tbuf + strlen(tbuf);
			while (dot > tbuf && *dot != '.') dot--;
			if (dot > tbuf) *dot = 0;
			bcpy(title, sizeof title, tbuf);
		}
		lyric_start(current, filename, artist, title, is_local);
	}
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
	lyric_invalidate(); // drop this track's lyrics + any in-flight fetch result
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
	// Captured unconditionally now: audio-only tracks need the same clock to
	// drive the LRC lyrics display.
	video_clock_capture(xmpfstatus ? xmpfstatus->GetTime() : f->pos);
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
	if (f->vstream >= 0) {
		video_flush();
		{
			EnterCriticalSection(&video.lock);
			video.seeking = TRUE; // show black until the catch-up reaches the target
			LeaveCriticalSection(&video.lock);
		}
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
		"FFmpeg video / lyrics", // visname (B2: video shows in XMPlay's vis area;
		                         // audio-only tracks show LRC lyrics there instead)
		FF_VisOpen,
		FF_VisClose,
		FF_VisSize,
		FF_VisRender,
		FF_VisRenderDC,
		NULL,             // VisButton (fullscreen is the vis area's own double-click)
		FF_VisConfig,     // vis config dialog (lyrics service / colours / font)
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
	xmpfreg   = (XMPFUNC_REGISTRY*)faceproc(XMPFUNC_REGISTRY_FACE);
	xmpver = xmpfmisc->GetVersion();
	InitializeCriticalSection(&video.lock);
	InitializeCriticalSection(&demux_lock);
	lrc_cfg_load(); // lyrics settings from the registry face (defaults if absent)
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
		dllinst = hDLL;
		InitializeCriticalSection(&lyric.lock);
		InitializeCriticalSection(&lyric_thread_lock);
		break;
	case DLL_PROCESS_DETACH:
		if (ws_init_done) WSACleanup();
		if (lyric_fetch_handle) CloseHandle(lyric_fetch_handle);
		DeleteCriticalSection(&lyric_thread_lock);
		DeleteCriticalSection(&lyric.lock);
		break;
	}
	return TRUE;
}
#endif
