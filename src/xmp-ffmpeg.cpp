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

#define WM_APP_SHOW (WM_APP + 1) // posted window ops run on the window thread
#define WM_APP_HIDE (WM_APP + 2)

static XMPFUNC_IN     *xmpfin;
static XMPFUNC_MISC   *xmpfmisc;
static XMPFUNC_FILE   *xmpffile;
static XMPFUNC_TEXT   *xmpftext;
static XMPFUNC_STATUS *xmpfstatus;
static DWORD xmpver;
static HWND xmpwin; // XMPlay's main window - owner of the video window

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
};
static FFMPEG *current;

// --------------------------------------------------------------- video state

struct VPKT { VPKT *next; AVPacket *pkt; };

struct VIDEO {
	CRITICAL_SECTION lock;
	volatile BOOL active;      // a video stream is attached
	volatile BOOL running;     // render thread alive
	HWND hwnd;                 // window (owned by the render thread)
	DWORD threadid;
	HANDLE thread, ready;      // ready event: window created
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
	// persistent double buffer (render thread only), rebuilt on size change
	HDC memdc;
	HBITMAP membmp;
	HGDIOBJ memold;
	int memw, memh;
	AVFrame *vframe;
	BOOL have_frame;           // decoded frame in vframe, waiting for its time
	double frame_pts;
	BYTE *dib;                 // BGRA DIBits
	BITMAPINFO bmi;
	int dibw, dibh;
	BOOL fullscreen;
	WINDOWPLACEMENT saved;     // windowed placement before fullscreen
	BOOL saved_valid;
	BOOL hidden;
	char title[512];
	// playback clock: captured on XMPlay's thread, interpolated here. The
	// render thread must NEVER call XMPlay functions (deadlock risk).
	double clock_base;
	DWORD clock_tick;
	int show_w, show_h;
	BOOL size_set;           // default size applied once - NEVER resize the
	                         // user's window afterwards
	double last_pts, frame_dur;   // pts fallback for streams without stamps
	AVRational sar;               // non-square pixel ratio (display aspect)
	AVRational frame_sar;         // per-frame override when present
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
	if (filename && strstr(filename, "://")) {
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

	{ // filetype first
		const AVInputFormat *iformat = f->fmt->iformat;
		TAGAPPEND("filetype");
		TAGAPPEND(iformat && iformat->long_name ? iformat->long_name : "FFmpeg");
	}
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

static void video_paint(HDC dc)
{
	RECT rc;
	GetClientRect(video.hwnd, &rc);
	int cw = rc.right, ch = rc.bottom;
	if (cw < 1 || ch < 1) return;

	// double buffering: compose into an offscreen bitmap, blit once. Painting
	// black-then-frame straight to the window flickers visibly.
	if (!video.memdc || video.memw != cw || video.memh != ch) {
		if (video.memdc) {
			SelectObject(video.memdc, video.memold);
			DeleteObject(video.membmp);
			DeleteDC(video.memdc);
		}
		video.memdc = CreateCompatibleDC(dc);
		video.membmp = CreateCompatibleBitmap(dc, cw, ch);
		video.memold = SelectObject(video.memdc, video.membmp);
		video.memw = cw;
		video.memh = ch;
	}
	HDC mem = video.memdc;

	// clear the whole client: no stale pixels around the letterbox
	FillRect(mem, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

	if (video.dib && video.dibw >= 1) {
		// display aspect honors non-square pixels (DVD/TS/MKV anamorphic sources)
		double sar = 1.0;
		AVRational fsar = video.frame_sar;
		if (fsar.num <= 0 || fsar.den <= 0) fsar = video.sar;
		if (fsar.num > 0 && fsar.den > 0) sar = (double)fsar.num / fsar.den;

		int w, h, x, y;
		double va = (double)video.dibw / video.dibh * sar, wa = (double)cw / ch;
		if (wa > va) { h = ch; w = (int)(h * va + 0.5); x = (cw - w) / 2; y = 0; }
		else { w = cw; h = (int)(w / va + 0.5); x = 0; y = (ch - h) / 2; }
		if (w < 1) w = 1;
		if (h < 1) h = 1;

		// smooth (filtered) scaling
		int oldmode = SetStretchBltMode(mem, HALFTONE);
		StretchDIBits(mem, x, y, w, h, 0, 0, video.dibw, video.dibh,
			video.dib, &video.bmi, DIB_RGB_COLORS, SRCCOPY);
		SetStretchBltMode(mem, oldmode);
	}

	BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
}

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
	video.dibw = w; video.dibh = h;
	return video.dib != 0;
}

// present the decoded frame in vframe
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
	HDC dc = GetDC(video.hwnd);
	if (dc) { video_paint(dc); ReleaseDC(video.hwnd, dc); }
}

// -- sync tick: decode/present frames due at the playback position.
// BOUNDED work per call: without a budget, a broken timestamp stream (all
// frames "late") makes this loop decode forever while the audio thread keeps
// feeding packets - 100% CPU spin, starved audio, eventual crash.

static void video_tick()
{
	if (!video.active || video.hidden || !video.vdec) return;

	// companion behavior: when XMPlay's window becomes the foreground window
	// (clicked, restored from the taskbar, alt-tabbed to...), the video window
	// rides along and shows itself on top - the pair behaves as one app.
	if (xmpwin && GetForegroundWindow() == xmpwin)
		SetWindowPos(video.hwnd, HWND_TOP, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

	double t = video_clock_now(); // interpolated playback clock
	BOOL do_render = FALSE;

	EnterCriticalSection(&video.lock);
	if (!video.active) { LeaveCriticalSection(&video.lock); return; }
	int budget = 8; // max decode/feed iterations this tick
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
		// have a decoded frame: show it when due, drop when far too late
		if (video.frame_pts > t + 0.06) break;   // in the future - wait
		if (video.frame_pts < t - 0.30 && budget > 2) { // behind - drop, but
			video.have_frame = FALSE;            // never burn the whole budget
			continue;
		}
		do_render = TRUE;
		video.have_frame = FALSE;
		break; // one present per tick is enough
	}
	LeaveCriticalSection(&video.lock);

	// heavy work (scale + blit) happens OUTSIDE the lock so the audio thread
	// never waits behind GDI
	if (do_render) video_render_frame();
}

// -- window

// Find XMPlay's main window. MUST be a window of our own process: the owner
// captured at plugin-load time can be ANY foreground window (e.g. Explorer),
// which makes the video window follow the wrong "parent" in z-order.
static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && !GetWindow(hwnd, 4 /* GW_OWNER */)
		&& hwnd != video.hwnd) {
		*(HWND*)lp = hwnd;
		return FALSE;
	}
	return TRUE;
}

static HWND find_xmpwin()
{
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (fg) GetWindowThreadProcessId(fg, &pid);
	if (fg && pid == GetCurrentProcessId() && fg != video.hwnd) return fg;
	HWND found = 0;
	EnumWindows(enum_proc, (LPARAM)&found);
	return found;
}

// Win11 rounds the corners of normal windows; our borderless style
// (WM_NCCALCSIZE=0) loses that. Ask DWM for rounded corners via the API,
// LOADED DYNAMICALLY - dwmapi.dll does not exist on XP and a dynamic load
// never shows up in the import table, so the XP audit stays clean.
static void video_round_corners(HWND hwnd)
{
	typedef long (WINAPI *DwmSetWindowAttribute_t)(HWND, DWORD, const void *, DWORD);
	HMODULE dwm = LoadLibraryA("dwmapi.dll");
	if (!dwm) return;
	DwmSetWindowAttribute_t pfn = (DwmSetWindowAttribute_t)GetProcAddress(dwm, "DwmSetWindowAttribute");
	if (pfn) {
		DWORD pref = 2; // DWMWCP_ROUND
		pfn(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &pref, sizeof(pref));
	}
}

static void video_toggle_fullscreen()
{
	video.fullscreen = !video.fullscreen;
	if (video.fullscreen) {
		video.saved.length = sizeof(video.saved);
		video.saved_valid = GetWindowPlacement(video.hwnd, &video.saved);
		HMONITOR mon = MonitorFromWindow(video.hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi; mi.cbSize = sizeof(mi);
		GetMonitorInfo(mon, &mi);
		SetWindowLong(video.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowPos(video.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
			mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
			SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	} else {
		SetWindowLong(video.hwnd, GWL_STYLE, WS_POPUP | WS_THICKFRAME | WS_VISIBLE);
		if (video.saved_valid) SetWindowPlacement(video.hwnd, &video.saved);
		else SetWindowPos(video.hwnd, 0, 0, 0, 640, 480, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
		SetWindowPos(video.hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	}
}

static LRESULT CALLBACK video_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_NCCALCSIZE: // the ENTIRE window is client area: no painted frame
		return 0;       // (WS_THICKFRAME stays only so HT* resizing works)
	case WM_NCPAINT:
		return 0;       // nothing non-client to paint - no white edge
	case WM_NCHITTEST: { // borderless: manual resize edges + drag + resize
		if (video.fullscreen) return HTCLIENT;
		POINT pt;
		pt.x = (short)((DWORD)lp & 0xffff);
		pt.y = (short)(((DWORD)lp >> 16) & 0xffff);
		RECT wr;
		GetWindowRect(hwnd, &wr);
		int b = 8; // resize border width in px
		BOOL l = pt.x < wr.left + b, r = pt.x >= wr.right - b;
		BOOL t = pt.y < wr.top + b, bo = pt.y >= wr.bottom - b;
		if (t && l) return HTTOPLEFT;
		if (t && r) return HTTOPRIGHT;
		if (bo && l) return HTBOTTOMLEFT;
		if (bo && r) return HTBOTTOMRIGHT;
		if (t) return HTTOP;
		if (bo) return HTBOTTOM;
		if (l) return HTLEFT;
		if (r) return HTRIGHT;
		return HTCAPTION; // drag anywhere else
	}
	case WM_NCLBUTTONDBLCLK: // double-click on the drag area (our client is
		if (wp == HTCAPTION) { // reported as caption!) = fullscreen toggle
			video_toggle_fullscreen();
			return 0;
		}
		break;
	case WM_LBUTTONDBLCLK:
		video_toggle_fullscreen();
		return 0;
	case WM_GETMINMAXINFO: { // keep a sane minimum size
		MINMAXINFO *mm = (MINMAXINFO*)lp;
		mm->ptMinTrackSize.x = 160;
		mm->ptMinTrackSize.y = 120;
		return 0;
	}
	case WM_KEYDOWN:
		if (wp == 'F') { video_toggle_fullscreen(); return 0; }
		if (wp == VK_ESCAPE) {
			if (video.fullscreen) video_toggle_fullscreen();
			else { video.hidden = TRUE; ShowWindow(hwnd, SW_HIDE); }
			return 0;
		}
		if (wp == VK_RETURN) { video_toggle_fullscreen(); return 0; }
		break;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		video_paint(dc); // double buffered: black + frame composed offscreen
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_ACTIVATE:
		// owned-window z-order only guarantees WE sit above XMPlay - clicking
		// us would leave XMPlay behind other apps' windows. Keep the owner
		// visible right below us (without stealing focus).
		if (LOWORD(wp) != 0 /* WA_INACTIVE */ && xmpwin)
			SetWindowPos(xmpwin, video.hwnd, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		break;
	case WM_ERASEBKGND:
		return 1;
	case WM_APP_SHOW: {
		if (xmpwin) SetWindowLong(video.hwnd, GWL_HWNDPARENT, (LONG)xmpwin);
		// size: default ONCE at the first show, never afterwards - the user's
		// window size (and fullscreen) is not to be touched during playback
		if (!video.size_set) {
			SetWindowPos(video.hwnd, HWND_TOP, 0, 0, video.show_w, video.show_h,
				SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
			video.size_set = TRUE;
		} else {
			SetWindowPos(video.hwnd, HWND_TOP, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
		}
		WCHAR wt[512];
		MultiByteToWideChar(CP_UTF8, 0, video.title, -1, wt, 512);
		SetWindowTextW(video.hwnd, wt);
		ShowWindow(video.hwnd, SW_SHOW);
		SetForegroundWindow(video.hwnd);
		return 0;
	}
	case WM_APP_HIDE:
		video.hidden = TRUE;
		ShowWindow(video.hwnd, SW_HIDE);
		return 0;
	case WM_TIMER:
		video_tick();
		return 0;
	case WM_SIZE:
		InvalidateRect(hwnd, 0, FALSE);
		return 0;
	}
	return DefWindowProc(hwnd, msg, wp, lp);
}

static DWORD WINAPI video_thread(LPVOID)
{
	WNDCLASSEX wc;
	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = video_wndproc;
	wc.hInstance = GetModuleHandle(0);
	wc.hCursor = LoadCursor(0, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wc.lpszClassName = "xmp-ffmpeg-video";
	RegisterClassEx(&wc);

	video.hwnd = CreateWindowEx(WS_EX_TOOLWINDOW, "xmp-ffmpeg-video", "xmp-ffmpeg",
		WS_POPUP | WS_THICKFRAME, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
		xmpwin, 0, wc.hInstance, 0); // owner = XMPlay main window
	SetTimer(video.hwnd, 1, 15, 0);
	video_round_corners(video.hwnd);
	SetEvent(video.ready);

	MSG msg;
	while (GetMessage(&msg, 0, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return 0;
}

static void video_window_init()
{
	if (video.running) return;
	video.clock_base = 0;
	video.clock_tick = GetTickCount();
	video.ready = CreateEvent(0, TRUE, FALSE, 0);
	video.thread = CreateThread(0, 0, video_thread, 0, 0, &video.threadid);
	if (video.ready) WaitForSingleObject(video.ready, 3000);
	video.running = TRUE;
}

static void video_show(FFMPEG *f)
{
	video_window_init();
	if (!video.hwnd) return;
	// default size = native video size, clamped to the work area
	int w = f->vw > 0 ? f->vw : 640, h = f->vh > 0 ? f->vh : 480;
	RECT wa; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
	int maxw = (int)((wa.right - wa.left) * 0.85), maxh = (int)((wa.bottom - wa.top) * 0.85);
	while (w > maxw || h > maxh) { w = w * 9 / 10; h = h * 9 / 10; }
	// ALL window operations happen on the window thread (posted, never
	// cross-thread SendMessage) - see WM_APP_SHOW below.
	video.show_w = w;
	video.show_h = h;
	video.hidden = FALSE;
	xmpwin = find_xmpwin(); // owner must be XMPlay itself, not whatever had focus at load
	PostMessage(video.hwnd, WM_APP_SHOW, 0, 0);
}

// called after a successful Open: attach this file's video to the window
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
	video.active = TRUE;
	LeaveCriticalSection(&video.lock);
	video_show(f);
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
	LeaveCriticalSection(&video.lock);
	if (video.hwnd) PostMessage(video.hwnd, WM_APP_HIDE, 0, 0);
}

// ============================================================= XMPIN methods

static void WINAPI FF_About(HWND win)
{
	char msg[256];
	sprintf(msg, "%s %s\n\nPlays any audio format FFmpeg can decode,\n"
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
	// window title (stored UTF-8)
	{
		char t[460];
		snprintf(t, sizeof(t), "%s - " PLUGIN_NAME, filename ? filename : "");
		strncpy(video.title, t, sizeof(video.title) - 1);
		video.title[sizeof(video.title) - 1] = 0;
	}
	video_attach(current); // shows the window iff this file has video
	return 1;
}

static void WINAPI FF_Close()
{
	video_detach(); // stop the render thread before freeing the video decoder
	FreeFF(current);
	current = 0;
}

static char *WINAPI FF_GetTags()
{
	return current ? BuildTags(current) : NULL;
}

static void WINAPI FF_SetFormat(XMPFORMAT *form)
{
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
	FFMPEG *f = current;
	if (!f) return;
	const AVInputFormat *iformat = f->fmt->iformat;
	buf += sprintf(buf, "Format\t%s\r", iformat && iformat->long_name ? iformat->long_name : "?");
	buf += sprintf(buf, "Audio codec\t%s\r", f->codec ? f->codec : "?");
	if (f->vstream >= 0)
		buf += sprintf(buf, "Video\t%s %dx%d\r", f->video_codec ? f->video_codec : "?", f->vw, f->vh);
	if (f->bitrate) buf += sprintf(buf, "Bit rate\t%u kbps\r", (unsigned)f->bitrate);
	buf += sprintf(buf, "Sample rate\t%d hz\rChannels\t%d\r",
		f->dec->sample_rate, f->dec->ch_layout.nb_channels);
}

static void WINAPI FF_GetMessage(char *buf)
{
	FFMPEG *f = current;
	if (!f) return;
	AVDictionary *md[2] = { f->fmt->streams[f->stream]->metadata, f->fmt->metadata };
	for (int i = 0; i < 2; i++) {
		AVDictionaryEntry *e = NULL;
		while ((e = av_dict_get(md[i], "", e, AV_DICT_IGNORE_SUFFIX)))
			buf = xmpfmisc->FormatInfoText(buf, e->key, e->value);
	}
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
	if (video.active) video_clock_capture(xmpfstatus->GetTime());
	DWORD done = 0;
	int ch = f->dec->ch_layout.nb_channels;
	if (ch < 1) ch = 1;

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
			avcodec_send_packet(f->dec, f->pkt);
			av_packet_unref(f->pkt);
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
	if (!f->seekable) { logmsg("seek: not seekable (pos=%lu t=%.3f)", (unsigned long)pos, t); return -1; }

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
	video_flush(); // reposition the video too
	video_clock_capture(t);
	f->pos = t;
	return t;
}

// ------------------------------------------------------------------ exported

extern "C" __declspec(dllexport) XMPIN *WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
	static XMPIN xmpin = {
		XMPIN_FLAG_CANSTREAM | XMPIN_FLAG_MULTIEXT | XMPIN_FLAG_NETSEEK,
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
		// no built-in visualisation
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
	xmpwin = find_xmpwin(); // refreshed again at each show
	InitializeCriticalSection(&video.lock);
	video.ready = 0;
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
			PLUGIN_NAME, PLUGIN_VER, (unsigned long)(XMPIN_FLAG_CANSTREAM | XMPIN_FLAG_MULTIEXT | XMPIN_FLAG_NETSEEK),
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
