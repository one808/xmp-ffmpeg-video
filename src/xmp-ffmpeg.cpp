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
#include <d3d9.h> // video window renders via D3D (bypasses GDI); d3d9.dll is
#include <wchar.h>        // loaded dynamically, so no d3d9 import (XP audit clean)
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
// the plugin's own borderless video window: the picture renders HERE on the
// plugin's own window thread, decoupled from XMPlay's audio/UI so the GDI
// blit can't choke the audio. All window ops are posted (never cross-thread
// SendMessage) so they run on the window thread's own message loop.
#define WM_APP_VSHOW   (WM_APP + 1) // show + dock to XMPlay (audio thread -> window)
#define WM_APP_VHIDE   (WM_APP + 2) // hide (stop / switch)
#define WM_APP_VBLANK  (WM_APP + 3) // clear the picture to black
#define WM_APP_VPRESENT (WM_APP + 4) // a fresh frame is in the DIB: repaint
// bisection switch ("FFmpeg video"/VideoMode in XMPlay's registry face):
//   0 = full video in the plugin's own D3D window (default). Audio-priority:
//       when the window is hidden (ESC / audio-only) the D3D device is released
//       so the audio path returns to pure-audio behavior; F / double-click
//       switches to EXCLUSIVE fullscreen (no DWM) for the cleanest audio
//       while actually watching.
//   1 = video attached but NO decode thread
//   2 = play as audio-only (no video attach; LRC lyrics show instead)
//   3 = decode runs, but the GDI blit to the vis area is skipped (picture
//       not shown) - isolates the GDI render from the decode CPU
//   4 = DIAG: own window shown, presents throttled to ~1fps (isolates the
//       PRESENT RATE from the audio chop)
//   5 = DIAG: own window shown, only the FIRST frame is presented (isolates
//       window existence from per-frame present activity)
// Used to isolate which part of the video path chokes the audio on video tracks.
static int g_video_mode = 0;

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
// the render DIB (and thus the sws_scale target) is capped at this long side:
// scaling 1080p/4K to full res every frame is the decode thread's main CPU
// cost, and its periodic bursts jitter the audio pump (audio chop on video
// tracks). 720p-class output is plenty for the vis area; StretchDIBits ups it.
#define VIDEO_DIB_MAX 1280

// --------------------------------------------------------------- video state

struct VPKT { VPKT *next; AVPacket *pkt; };

struct VIDEO {
	CRITICAL_SECTION lock;
	CRITICAL_SECTION qlock;     // fast packet-queue lock: audio enqueue / video
	                             // pop. Kept separate from `lock` so the audio
	                             // pump never blocks on the video thread's heavy
	                             // decode+scale critical section.
	CRITICAL_SECTION clocklock; // the playback clock (clock_base/clock_tick),
	                             // captured on the audio thread every pump call -
	                             // kept off `lock` so the audio pump never waits
	                             // on the video thread's frame scale.
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
	volatile BOOL vis_open;     // vis area open (FF_VisOpen / FF_VisClose)
	int sws_tw, sws_th;         // sws TARGET (DIB) size - capped for CPU, not full res
	// independent borderless video window (the picture lives here, not in the
	// XMPlay vis area - so the GDI blit is off XMPlay's threads)
	HWND hwnd;                  // the video window (created on the window thread)
	HANDLE wnd_thread;          // the window thread (owns hwnd + the message loop)
	DWORD wnd_threadid;
	volatile BOOL win_visible;  // window shown: the decode thread only works when this
	HDC memdc;                  // double-buffered paint (window thread only)
	HBITMAP membmp;
	HGDIOBJ memold;
	int memw, memh;
	BOOL fullscreen;
	WINDOWPLACEMENT saved;
	BOOL saved_valid;
	volatile BOOL hidden;       // user hid it (ESC) - decode idles
	int show_w, show_h;         // default window size (native video, clamped)
	BOOL size_set;              // default size applied once, never fought afterwards
	long present_throttle_ms;   // diagnostic modes 4/5: cap the present rate (0 = full fps)
	DWORD last_present_ms;      // GetTickCount() when the last present ran
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

// persistent log handle: opened once and kept open, guarded by a lock. The
// old per-call fopen("a")+fclose ran on the audio pump thread every 100ms and
// was a source of I/O jitter - a contributor to the "audio choppiness on
// video tracks". A shared, always-open handle makes logmsg cheap and safe.
static FILE *g_logf = 0;
static CRITICAL_SECTION g_loglock;

static void log_open(void)
{
	if (g_logf) return;
	char dir[300];
	if (!GetTempPathA(300, dir)) return;
	strcat(dir, "xmp-ffmpeg-video.log");
	g_logf = fopen(dir, "a");
}
static void logmsg(const char *fmt, ...)
{
	EnterCriticalSection(&g_loglock);
	log_open();
	if (g_logf) {
		va_list ap;
		va_start(ap, fmt);
		vfprintf(g_logf, fmt, ap);
		va_end(ap);
		fputc('\n', g_logf);
	}
	LeaveCriticalSection(&g_loglock);
}

// route FFmpeg's own log into ours - network/TLS failures then show up with
// their real cause (403, handshake, DNS, ...)
static void ff_log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
	if (level > AV_LOG_INFO) return;
	EnterCriticalSection(&g_loglock);
	log_open();
	if (g_logf) {
		fputs("  ff: ", g_logf);
		vfprintf(g_logf, fmt, vl);
		fputc('\n', g_logf);
	}
	LeaveCriticalSection(&g_loglock);
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
				if (f->vdec) {
					// single-threaded video decode: FFmpeg's frame-threading worker
					// pool (N threads waking per frame + atomic sync) is a classic
					// source of audio-pump jitter. One decode thread is slower per
					// frame but far less invasive on the scheduler - audio stays smooth.
					f->vdec->thread_count = 1;
				}
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
	EnterCriticalSection(&video.qlock);
	if (video.qcount > 600) { // bounded queue: drop the new packet instead of
		LeaveCriticalSection(&video.qlock); // growing without limit
		av_packet_free(&pkt);
		free(n);
		return;
	}
	if (video.qtail) video.qtail->next = n; else video.qhead = n;
	video.qtail = n;
	video.qcount++;
	LeaveCriticalSection(&video.qlock);
}

static void video_flush()
{
	// queue under the fast lock; decoder state under the display lock. Taken
	// sequentially (never nested) to keep the two locks independent.
	EnterCriticalSection(&video.qlock);
	VPKT *p = video.qhead;
	while (p) { VPKT *n = p->next; av_packet_free(&p->pkt); free(p); p = n; }
	video.qhead = video.qtail = 0;
	video.qcount = 0;
	LeaveCriticalSection(&video.qlock);
	EnterCriticalSection(&video.lock);
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
	EnterCriticalSection(&video.clocklock);
	video.clock_base = t;
	video.clock_tick = GetTickCount();
	LeaveCriticalSection(&video.clocklock);
}

static double video_clock_now()
{
	EnterCriticalSection(&video.clocklock);
	double t = video.clock_base + (double)(GetTickCount() - video.clock_tick) * 0.001;
	LeaveCriticalSection(&video.clocklock);
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
// XMPlay's own visualisation area. The DIB is sized to the DISPLAY, not the
// source: full-res (1080p/4K) sws_scale every frame is the decode thread's
// main CPU cost, and its periodic bursts are what jitters the audio pump on
// video tracks. Capping the target to VIDEO_DIB_MAX makes the scale cheap;
// FF_VisRenderDC's StretchDIBits then ups it to the vis area.
static void video_render_frame()
{
	AVFrame *fr = video.vframe;
	if (fr->width < 1 || fr->height < 1) return;
	// target DIB size: the source, scaled down so its long side <= VIDEO_DIB_MAX
	long long L = (fr->width > fr->height) ? fr->width : fr->height;
	int sw = fr->width, sh = fr->height;
	if (L > VIDEO_DIB_MAX) {
		double r = (double)VIDEO_DIB_MAX / (double)L;
		sw = (int)(fr->width * r + 0.5);
		sh = (int)(fr->height * r + 0.5);
	}
	sw &= ~1; sh &= ~1; // YUV420P scale wants even dims
	if (sw < 2) sw = 2;
	if (sh < 2) sh = 2;
	if (!video_make_dib(sw, sh)) return;
	if (!video.sws || video.sws_w != fr->width || video.sws_h != fr->height
		|| video.sws_fmt != fr->format || video.sws_tw != sw || video.sws_th != sh) {
		if (video.sws) sws_freeContext(video.sws);
		video.sws = sws_getContext(fr->width, fr->height, (AVPixelFormat)fr->format,
			sw, sh, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
		video.sws_w = fr->width; video.sws_h = fr->height; video.sws_fmt = fr->format;
		video.sws_tw = sw; video.sws_th = sh;
		if (!video.sws) return;
	}
	uint8_t *dst[4] = { video.dib, 0, 0, 0 };
	int ls[4] = { sw * 4, 0, 0, 0 };
	sws_scale(video.sws, fr->data, fr->linesize, 0, fr->height, dst, ls);
	video.have_new = TRUE; // a fresh frame is waiting for FF_VisRenderDC
}

// present/paint latency stats, written by the window thread, reported (and
// reset) by the decode thread's vt: heartbeat: pr= count, prw= worst us.
static LARGE_INTEGER g_prqf;
static volatile long g_pr_worst_us;
static volatile long g_pr_count;
static void pr_stat(LARGE_INTEGER p0, LARGE_INTEGER p1)
{
	g_pr_count++;
	if (g_prqf.QuadPart) {
		long us = (long)((double)(p1.QuadPart - p0.QuadPart) * 1000000.0
			/ (double)g_prqf.QuadPart);
		if (us > g_pr_worst_us) g_pr_worst_us = us;
	}
}

// decode the frame(s) due at the playback position into video.dib.
// BOUNDED work per call: without a budget, a broken-timestamp stream (all
// frames "late") makes this loop decode forever - 100% CPU spin, starved audio.
static void video_tick()
{
	if (!video.active || !video.vdec) return;
	double t = video_clock_now();
	int budget = 8; // max decode/feed iterations this tick
	int nstarve = 0, ndrop = 0;
	double shown_pts = -1;
	// per-tick CPU: how many ms this tick's decode+scale actually took, so the
	// vt: heartbeat can report the decode thread's real load (busy total + the
	// worst single tick per second). Answers "is the audio chop the decode
	// thread's CPU, or something else?"
	static LARGE_INTEGER s_qf = {0};
	if (s_qf.QuadPart == 0) {
		if (!QueryPerformanceFrequency(&s_qf) || s_qf.QuadPart == 0)
			s_qf.QuadPart = 10000000; // fallback: 10MHz
	}
	LARGE_INTEGER q0; QueryPerformanceCounter(&q0);
	unsigned long work_ms = 0;
	// The heavy avcodec decode + sws_scale now run OUTSIDE the display lock:
	// only the packet pop takes the fast queue lock, and only the dib/flag
	// bookkeeping takes video.lock. So the audio thread's video_enqueue
	// (qlock) is never held up by a decode/scale burst - the root cause of the
	// "video track -> audio choppiness" underrun.
	while (budget-- > 0) {
		if (!video.have_frame) {
			int ret = avcodec_receive_frame(video.vdec, video.vframe);
			if (ret == AVERROR(EAGAIN)) {
				VPKT *n = 0;
				{
					EnterCriticalSection(&video.qlock);
					if (video.qhead) {
						n = video.qhead;
						video.qhead = n->next;
						if (!video.qhead) video.qtail = 0;
						video.qcount--;
					}
					LeaveCriticalSection(&video.qlock);
				}
				if (n) {
					if (avcodec_send_packet(video.vdec, n->pkt) == AVERROR(EAGAIN)) {
						// decoder full - put the packet back, drain first
						EnterCriticalSection(&video.qlock);
						n->next = video.qhead;
						video.qhead = n;
						if (!video.qtail) video.qtail = n;
						video.qcount++;
						LeaveCriticalSection(&video.qlock);
						break;
					}
					av_packet_free(&n->pkt);
					free(n);
					continue;
				}
				{ // queue empty: drain marker at demux EOF, else starve
					EnterCriticalSection(&video.lock);
					if (video.demux_eof_seen && !video.v_eof_sent) {
						video.v_eof_sent = TRUE;
						LeaveCriticalSection(&video.lock);
						avcodec_send_packet(video.vdec, NULL);
						continue;
					}
					LeaveCriticalSection(&video.lock);
				}
				nstarve++;
				break; // starved - wait for more packets
			}
			if (ret < 0) break; // drained or error
			EnterCriticalSection(&video.lock);
			{
				double pts;
				if (video.vframe->best_effort_timestamp != AV_NOPTS_VALUE)
					pts = video.vframe->best_effort_timestamp * av_q2d(video.tb);
				else
					pts = video.last_pts + video.frame_dur;
				if (pts > video.last_pts) video.last_pts = pts;
				video.frame_pts = pts;
				video.frame_sar = video.vframe->sample_aspect_ratio;
			}
			LeaveCriticalSection(&video.lock);
			video.have_frame = TRUE;
			continue;
		}
		// have a decoded frame: show it when due, drop when far too late.
		EnterCriticalSection(&video.lock);
		if (video.frame_pts > t + 0.06) { // in the future - wait
			LeaveCriticalSection(&video.lock);
			break;
		}
		if (video.frame_pts < t - 0.30 && (budget > 2 || video.seeking)) { // behind - drop
			video.have_frame = FALSE;
			ndrop++;
			LeaveCriticalSection(&video.lock);
			continue;
		}
		video_render_frame(); // scale into video.dib; sets have_new
		shown_pts = video.frame_pts;
		if (video.seeking) video.seeking = FALSE; // catch-up done
		video.have_frame = FALSE;
		LeaveCriticalSection(&video.lock);
		break; // one present per tick is enough
	}
	if (shown_pts > 0 && video.hwnd)
		PostMessage(video.hwnd, WM_APP_VPRESENT, 0, 0); // repaint on the window thread
	{ // per-tick CPU: how long this tick's decode+scale took
		LARGE_INTEGER q1; QueryPerformanceCounter(&q1);
		work_ms = (unsigned long)(((double)(q1.QuadPart - q0.QuadPart) * 1000.0) / (double)s_qf.QuadPart);
	}
	EnterCriticalSection(&video.qlock);
	int qnow = video.qcount;
	LeaveCriticalSection(&video.qlock);
	// 1/sec diagnostic heartbeat: queue depth + per-second starvation/drop
	// counts + decode-thread CPU (busy total + worst single tick), used to
	// diagnose the post-seek judder and the audio chop (%TEMP%\xmp-ffmpeg-video.log)
	{
		static int s_starved = 0, s_dropped = 0;
		static unsigned long s_busy = 0, s_maxtick = 0;
		static double s_shown_pts = -1;
		if (shown_pts > 0) s_shown_pts = shown_pts;
		s_starved += nstarve;
		s_dropped += ndrop;
		s_busy += work_ms;
		if (work_ms > s_maxtick) s_maxtick = work_ms;
		DWORD nowt = GetTickCount();
		static DWORD s_last = 0;
		if (!s_last) s_last = nowt;
		if (nowt - s_last >= 1000) {
			s_last = nowt;
			logmsg("vt: q=%d starved=%d dropped=%d last_shown=%.2f clock=%.2f busy=%lums max=%lums pr=%ld prw=%ldus",
				qnow, s_starved, s_dropped, s_shown_pts, t, s_busy, s_maxtick,
				g_pr_count, g_pr_worst_us);
			s_starved = s_dropped = 0;
			s_busy = 0; s_maxtick = 0;
			g_pr_count = 0; g_pr_worst_us = 0;
		}
	}
}

// -- window (B1): the plugin's own borderless video window. The picture
// renders HERE on the plugin's own window thread, decoupled from XMPlay's
// audio/UI threads so the GDI blit can't choke the audio (the XMPlay-vis GDI
// path did exactly that - see the audio-chop bisection). The decode thread
// keeps decoding into the DIB and posts WM_APP_VPRESENT; the window thread
// repaints (double buffered) on its own message loop. All window ops are
// posted (never cross-thread SendMessage) so they run on the window thread.
static HWND xmpwin; // XMPlay's main window - the video window's owner

// double-buffered paint of the DIB into the video window's client area. Runs
// on the window thread only (WM_PAINT). Composes black + frame offscreen,
// then one BitBlt - no flicker.
static void video_paint(HDC dc)
{
	RECT rc;
	GetClientRect(video.hwnd, &rc);
	int cw = rc.right, ch = rc.bottom;
	if (cw < 1 || ch < 1) return;

	if (!video.memdc || video.memw != cw || video.memh != ch) {
		if (video.memdc) {
			SelectObject(video.memdc, video.memold);
			DeleteObject(video.membmp);
			DeleteDC(video.memdc);
		}
		video.memdc = CreateCompatibleDC(dc);
		video.membmp = CreateCompatibleBitmap(dc, cw, ch);
		video.memold = SelectObject(video.memdc, video.membmp);
		video.memw = cw; video.memh = ch;
	}
	HDC mem = video.memdc;
	RECT full = { 0, 0, cw, ch };
	FillRect(mem, &full, (HBRUSH)GetStockObject(BLACK_BRUSH)); // clear: no stale pixels around the bars

	if (video.dib && video.dibw >= 1) {
		// display aspect honors non-square pixels (DVD/TS/MKV anamorphic)
		double sar = 1.0;
		AVRational fsar = video.frame_sar;
		if (fsar.num <= 0 || fsar.den <= 0) fsar = video.sar;
		if (fsar.num > 0 && fsar.den > 0) sar = (double)fsar.num / fsar.den;
		double va = (double)video.dibw / video.dibh * sar, wa = (double)cw / ch;
		int w, h, x, y;
		if (wa > va) { h = ch; w = (int)(h * va + 0.5); x = (cw - w) / 2; y = 0; }
		else         { w = cw; h = (int)(w / va + 0.5); x = 0; y = (ch - h) / 2; }
		if (w < 1) w = 1;
		if (h < 1) h = 1;
		if (w == video.dibw && h == video.dibh) {
			// exact 1:1 fit: no GDI scaling, one cheap direct blit
			SetDIBitsToDevice(mem, x, y, w, h, 0, 0, 0, 0, video.dib, &video.bmi, DIB_RGB_COLORS);
		} else {
			// only when the window is resized off the DIB size: fast nearest
			// neighbour (value 7), not the slow HALFTONE dither
			int oldmode = SetStretchBltMode(mem, 7 /* STRETCH_NEARESTNEIGHBOR */);
			StretchDIBits(mem, x, y, w, h, 0, 0, video.dibw, video.dibh,
				video.dib, &video.bmi, DIB_RGB_COLORS, SRCCOPY);
			SetStretchBltMode(mem, oldmode);
		}
	}
	BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
}

// clear the picture to black (frozen frames must not survive stop/switch)
static void video_blank()
{
	if (!video.dib) return;
	memset(video.dib, 0, (size_t)video.dibw * video.dibh * 4);
	InvalidateRect(video.hwnd, 0, FALSE); // repaint on the window thread
}

// --------------------------------------------------------------- D3D renderer
// The video is blitted with Direct3D, NOT GDI: per-frame GDI (StretchDIBits /
// BitBlt) takes the global GDI lock and, on this machine, jitters the audio
// path. D3D uploads the frame to a texture and presents it on the GPU - no GDI
// lock, so the audio stays smooth. d3d9.dll is loaded DYNAMICALLY (never in
// the import table) so the XP-compat audit stays clean; if D3D is unavailable
// we fall back to the GDI paint (video_paint).
static HMODULE d3d_mod = 0;
static IDirect3D9 *d3d = 0;
static IDirect3DDevice9 *d3dev = 0;
static IDirect3DTexture9 *d3tex = 0;
static int d3tex_w = 0, d3tex_h = 0;
static D3DPRESENT_PARAMETERS d3pp;
static volatile int s_dv_first = 0; // first successful D3D present (one-time log)

static void d3d_release(void)
{
	if (d3tex) { d3tex->Release(); d3tex = 0; }
	if (d3dev) { d3dev->Release(); d3dev = 0; }
	if (d3d)   { d3d->Release(); d3d = 0; }
	if (d3d_mod) { FreeLibrary(d3d_mod); d3d_mod = 0; }
}

static BOOL d3d_ensure_tex(int w, int h);
static void d3d_prime(void); // prime the driver's first present cycle (defined below)

static BOOL d3d_init(HWND hwnd)
{
	if (d3dev) return TRUE;
	d3d_mod = LoadLibraryA("d3d9.dll");
	if (!d3d_mod) return FALSE;
	typedef IDirect3D9 *(WINAPI *fnD3DCreate9_t)(UINT);
	fnD3DCreate9_t pfn = (fnD3DCreate9_t)GetProcAddress(d3d_mod, "Direct3DCreate9");
	if (!pfn) { d3d_release(); return FALSE; }
	d3d = pfn(D3D_SDK_VERSION);
	if (!d3d) { d3d_release(); return FALSE; }

	memset(&d3pp, 0, sizeof d3pp);
	d3pp.Windowed = TRUE;
	d3pp.hDeviceWindow = hwnd;
	// COPY is the universally-supported windowed swap effect: FLIP is a
	// fullscreen-only effect on most drivers and a windowed FLIP device
	// creation silently fails - which would drop us onto the GDI fallback
	// (and the GDI lock is exactly what we are trying to avoid).
	d3pp.SwapEffect = D3DSWAPEFFECT_COPY;
	d3pp.BackBufferCount = 1;
	// windowed back-buffer format. The legacy d3d9.h in this toolchain lacks the
	// GetAdapterFormat/GetAdapterFormatCount query methods, and X8R8G8B8 windowed
	// is universally supported, so set it directly.
	d3pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	if (d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd,
		D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3pp, &d3dev) != D3D_OK) {
		d3d_release();
		return FALSE;
	}
	d3d_prime(); // run the driver's first present cycle (see d3d_prime)
	return TRUE;
}

// Prime the device: render one empty (black) frame and Present it. This
// completes the driver's initial present cycle so that the first real
// texture upload is not blocked on it. (With a SYSTEMMEM texture the lock
// does not deadlock, but the first present still warms up the back buffer
// and the compositing path - without this, the first real frame can arrive
// a beat late or the surface stays uninitialised on some drivers.)
static void d3d_prime(void)
{
	if (!d3dev) return;
	HRESULT tl = d3dev->TestCooperativeLevel();
	if (tl == D3DERR_DEVICELOST || tl == D3DERR_DEVICENOTRESET)
		if (d3dev->Reset(&d3pp) != D3D_OK) return;
	d3dev->SetRenderTarget(0, 0);
	d3dev->BeginScene();
	d3dev->Clear(0, 0, D3DCLEAR_TARGET, 0xFF000000, -1.0f, 0);
	d3dev->EndScene();
	HRESULT ph = d3dev->Present(0, 0, 0, 0);
	logmsg("dv: d3d_prime present hr=0x%08X", (unsigned)ph);
}

static BOOL d3d_ensure_tex(int w, int h)
{
	if (!d3dev) return FALSE;
	if (d3tex && d3tex_w == w && d3tex_h == h) return TRUE;
	if (d3tex) { d3tex->Release(); d3tex = 0; }
	// SYSTEMMEM, not DEFAULT: a DEFAULT-pool texture's LockRect fails with
	// D3DERR_WASGATOCYCL until the driver's present cycle has completed at
	// least once - and before the first present it never completes
	// (upload needs the lock, the lock needs a finished present cycle:
	// deadlock; the window stays transparent). A system-memory texture is
	// locked by the CPU directly (no GPU cycle involved); the driver moves
	// it to the GPU at Present. 720p-class uploads are trivial either way.
	if (d3dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &d3tex, 0) != D3D_OK)
		return FALSE;
	d3tex_w = w; d3tex_h = h;
	return TRUE;
}

// upload the current DIB (BGRA, top-down) to a texture and present it, letter-
// boxed into the window. Runs on the window thread (from WM_APP_VPRESENT).
static void d3d_present(void)
{
	if (!d3dev || !video.dib || video.dibw < 1 || !video.dibh) return;

	// a display / resolution change loses the device: reset it (the texture
	// dies with the device and is recreated on demand). The driver also
	// auto-loses the device when the window is resized (VSHOW sizes the
	// window to the native video size) - and it may still be busy, so a
	// single Reset attempt often fails. A FAILED reset leaves the device
	// LOST and every subsequent present becomes a no-op: the window then
	// shows no pixels at all (it looks "transparent"). Retry with short
	// waits instead of giving up for the whole stream.
	HRESULT tl = d3dev->TestCooperativeLevel();
	if (tl == D3DERR_DEVICELOST || tl == D3DERR_DEVICENOTRESET) {
		BOOL ok = FALSE;
		for (int i = 0; i < 8; i++) {
			RECT rc;
			GetClientRect(video.hwnd, &rc);
			d3pp.BackBufferWidth = (UINT)(rc.right - rc.left);
			d3pp.BackBufferHeight = (UINT)(rc.bottom - rc.top);
			if (d3dev->Reset(&d3pp) == D3D_OK) { ok = TRUE; break; }
			Sleep(5);
		}
		if (!ok) {
			static DWORD s_lost_log = 0;
			DWORD now = GetTickCount();
			if (now - s_lost_log >= 1000) {
				s_lost_log = now;
				logmsg("dv: D3D device lost, Reset failing (win=%ux%u tex=%dx%d)",
					d3pp.BackBufferWidth, d3pp.BackBufferHeight, d3tex_w, d3tex_h);
			}
			return;
		}
		if (d3tex) { d3tex->Release(); d3tex = 0; }
		d3d_prime(); // re-run the first present cycle after the reset
	}
	if (!d3d_ensure_tex(video.dibw, video.dibh)) return;

	{ // copy the DIB into the texture (row by row; the pitch may differ)
		D3DLOCKED_RECT lr;
		// DONOTWAIT fails with D3DERR_WASGATOCYCL (0x8876086C) while the
		// texture is in the driver's present cycle - and BEFORE the first
		// successful present that cycle never drains, so DONOTWAIT fails
		// forever and the window gets no pixels (looks transparent). The
		// blocking variant (flags=0; there is no D3DLOCK_WAIT in D3D9)
		// breaks the deadlock: it waits out the cycle, at most ~1 frame,
		// on this LOWEST-priority window thread - never on the audio.
		HRESULT lh = d3tex->LockRect(0, &lr, 0, D3DLOCK_DONOTWAIT);
		if (lh != D3D_OK)
			lh = d3tex->LockRect(0, &lr, 0, 0);
		if (lh != D3D_OK) {
			static DWORD s_lock_log = 0;
			DWORD now = GetTickCount();
			if (now - s_lock_log >= 1000) {
				s_lock_log = now;
				logmsg("dv: LockRect failing hr=0x%08X coop=0x%08X", (unsigned)lh, (unsigned)tl);
			}
			return;
		}
		int w = video.dibw, h = video.dibh;
		const BYTE *src = video.dib;
		BYTE *dst = (BYTE*)lr.pBits;
		for (int y = 0; y < h; y++)
			memcpy(dst + (size_t)y * lr.Pitch, src + (size_t)y * w * 4, (size_t)w * 4);
		d3tex->UnlockRect(0);
	}

	// letterbox the video into the window client, in NDC
	int cw, ch;
	{ RECT rc; GetClientRect(video.hwnd, &rc); cw = rc.right; ch = rc.bottom; }
	if (cw < 1 || ch < 1) return;
	double sar = 1.0;
	AVRational fsar = video.frame_sar;
	if (fsar.num <= 0 || fsar.den <= 0) fsar = video.sar;
	if (fsar.num > 0 && fsar.den > 0) sar = (double)fsar.num / fsar.den;
	double va = (double)video.dibw / video.dibh * sar, wa = (double)cw / ch;
	int vw = cw, vh = ch, vx = 0, vy = 0;
	if (wa > va) { vw = (int)(ch * va + 0.5); vx = (cw - vw) / 2; }
	else          { vh = (int)(cw / va + 0.5); vy = (ch - vh) / 2; }
	float L = (float)(2.0 * vx / cw - 1.0);
	float R = (float)(2.0 * (vx + vw) / cw - 1.0);
	float T = (float)(1.0 - 2.0 * vy / ch);
	float B = (float)(1.0 - 2.0 * (vy + vh) / ch);

	IDirect3DSurface9 *bb = 0;
	// the legacy d3d9.h lacks D3DBACKBUFFER_TYPE_DEFAULT / the D3DTS_/D3DTOP_/
	// D3DTEXF_ enums: use the raw values (D3DBACKBUFFER_TYPE_BACK=1,
	// D3DTS_TEXTUREOP=0/D3DTS_TFACTORY=1, D3DTOP_SELECTARG=1, D3DTEXF_TEXTURE=0)
	if (d3dev->GetBackBuffer(0, 0, (D3DBACKBUFFER_TYPE)1 /*BACK*/, &bb) != D3D_OK) return;
	d3dev->SetRenderTarget(0, bb);
	{
		D3DVIEWPORT9 vp = { 0, 0, (UINT)cw, (UINT)ch, 0.0f, 1.0f };
		d3dev->SetViewport(&vp);
	}
	d3dev->BeginScene();
	d3dev->Clear(0, 0, D3DCLEAR_TARGET, 0xFF000000, -1.0f, 0);
	d3dev->SetTexture(0, d3tex);
	d3dev->SetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)0 /*TEXTUREOP*/, 1 /*SELECTARG*/);
	d3dev->SetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)1 /*TFACTORY*/, 0 /*TEXF_TEXTURE*/);
	// legacy header lacks D3DFVF_XYZRHW1: it is D3DFVF_XYZRHW plus a 32-bit
	// render-height flag
	d3dev->SetFVF(D3DFVF_XYZRHW | 0x20 /*32-bit RH*/);
	{
		struct V { float x, y, z, rh; float u, v; };
		V q[4] = {
			{ L, T, 0, 1, 0, 0 },
			{ R, T, 0, 1, 1, 0 },
			{ L, B, 0, 1, 0, 1 },
			{ R, B, 0, 1, 1, 1 },
		};
		d3dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
	}
	d3dev->EndScene();
	HRESULT ph = d3dev->Present(0, 0, 0, 0);
	if (bb) bb->Release();
	if (ph != D3D_OK) {
		static DWORD s_pres_log = 0;
		DWORD now = GetTickCount();
		if (now - s_pres_log >= 1000) {
			s_pres_log = now;
			logmsg("dv: Present failing hr=0x%08X", (unsigned)ph);
		}
	} else if (!s_dv_first) {
		s_dv_first = 1;
		logmsg("dv: first D3D present ok back=%ux%u tex=%dx%d",
			d3pp.BackBufferWidth, d3pp.BackBufferHeight, d3tex_w, d3tex_h);
	}
}

// find XMPlay's main top-level window (the video window's owner so it follows
// the player in z-order). Prefer the foreground window when it's ours; else
// the first visible top-level window in our process without an owner.
static BOOL CALLBACK video_enum_proc(HWND hwnd, LPARAM lp)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)
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
	EnumWindows(video_enum_proc, (LPARAM)&found);
	return found;
}

// Win11 rounds normal-window corners; our borderless style loses that. Ask
// DWM for rounded corners, LOADED DYNAMICALLY - dwmapi.dll does not exist on
// XP and a dynamic load never shows up in the import table, so the XP audit
// stays clean.
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

// exclusive-fullscreen D3D: reset the device into D3D's OWN fullscreen
// mode - the video surface stops going through DWM composition (the
// DWM-coupled present is what jitters the audio on this machine). The GDI
// window stays VISIBLE (borderless, topmost, full-screen) only so it keeps
// keyboard focus and can be left with F / ESC; the exclusive D3D surface
// renders on top of it, so the window's own pixels are never what you see.
// If the driver refuses exclusive (some multi-monitor / virtual setups),
// the caller falls back to the old borderless-windowed fullscreen.
static BOOL video_fullscreen_exclusive_on()
{
	if (!d3dev) return FALSE;
	D3DPRESENT_PARAMETERS fspp;
	memcpy(&fspp, &d3pp, sizeof fspp);
	fspp.Windowed = FALSE;
	fspp.hDeviceWindow = 0;
	fspp.SwapEffect = D3DSWAPEFFECT_FLIP; // fullscreen: FLIP is supported
	fspp.BackBufferCount = 1;
	fspp.BackBufferFormat = D3DFMT_X8R8G8B8;
	fspp.BackBufferWidth = (UINT)GetSystemMetrics(SM_CXSCREEN);
	fspp.BackBufferHeight = (UINT)GetSystemMetrics(SM_CYSCREEN);
	fspp.FullScreen_RefreshRateInHertz = 0; // keep the display's current mode
	if (d3tex) { d3tex->Release(); d3tex = 0; } // dies with the reset
	if (d3dev->Reset(&fspp) != D3D_OK) {
		d3dev->Reset(&d3pp); // back to windowed
		return FALSE;
	}
	d3pp = fspp; // the device-lost recovery path must reset to THESE params
	// keep the window visible, borderless, topmost, covering the primary
	// screen - it holds focus (so F/ESC still work); the exclusive surface
	// draws over it.
	SetWindowLong(video.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
	SetWindowLong(video.hwnd, GWL_EXSTYLE,
		GetWindowLong(video.hwnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
	HMONITOR mon = MonitorFromWindow(video.hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi; mi.cbSize = sizeof(mi);
	GetMonitorInfo(mon, &mi);
	SetWindowPos(video.hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
		mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
		SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
	d3d_prime();
	logmsg("dv: exclusive fullscreen on (%ux%u)",
		fspp.BackBufferWidth, fspp.BackBufferHeight);
	return TRUE;
}

static void video_fullscreen_exclusive_off()
{
	if (!d3dev || d3pp.Windowed) return;
	D3DPRESENT_PARAMETERS wspp;
	memcpy(&wspp, &d3pp, sizeof wspp);
	wspp.Windowed = TRUE;
	wspp.hDeviceWindow = video.hwnd;
	wspp.SwapEffect = D3DSWAPEFFECT_COPY;
	wspp.BackBufferWidth = 0; // 0 = keep the current (windowed) size
	wspp.BackBufferHeight = 0;
	if (d3tex) { d3tex->Release(); d3tex = 0; }
	if (d3dev->Reset(&wspp) != D3D_OK) { d3pp.Windowed = TRUE; return; }
	d3pp = wspp;
	SetWindowLong(video.hwnd, GWL_STYLE, WS_POPUP | WS_THICKFRAME | WS_VISIBLE);
	SetWindowLong(video.hwnd, GWL_EXSTYLE,
		GetWindowLong(video.hwnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST);
	d3d_prime();
	logmsg("dv: exclusive fullscreen off");
}

static void video_toggle_fullscreen()
{
	if (!video.hwnd) return;
	video.fullscreen = !video.fullscreen;
	if (video.fullscreen) {
		video.saved.length = sizeof(video.saved);
		video.saved_valid = GetWindowPlacement(video.hwnd, &video.saved);
		if (!video_fullscreen_exclusive_on()) {
			// the driver refused exclusive: borderless WINDOWED fullscreen
			// (DWM-composited - the old behavior)
			HMONITOR mon = MonitorFromWindow(video.hwnd, MONITOR_DEFAULTTONEAREST);
			MONITORINFO mi; mi.cbSize = sizeof(mi);
			GetMonitorInfo(mon, &mi);
			SetWindowLong(video.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
			SetWindowPos(video.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
				mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
				SWP_FRAMECHANGED | SWP_SHOWWINDOW);
			logmsg("dv: exclusive fullscreen refused -> windowed fallback");
		}
	} else {
		if (!d3pp.Windowed)
			video_fullscreen_exclusive_off(); // windowed D3D + restore style
		SetWindowLong(video.hwnd, GWL_STYLE, WS_POPUP | WS_THICKFRAME | WS_VISIBLE);
		if (video.saved_valid) SetWindowPlacement(video.hwnd, &video.saved);
		else SetWindowPos(video.hwnd, 0, 0, 0, 640, 480,
			SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
	}
	InvalidateRect(video.hwnd, 0, FALSE);
}

static LRESULT CALLBACK video_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_NCCALCSIZE: // the ENTIRE window is client area: no painted frame
		return 0;
	case WM_NCPAINT:
		return 0;
	case WM_NCHITTEST: { // borderless: manual resize edges + drag
		if (video.fullscreen) return HTCLIENT;
		POINT pt;
		pt.x = (short)(lp & 0xffff);
		pt.y = (short)(((DWORD)lp >> 16) & 0xffff);
		RECT wr; GetWindowRect(hwnd, &wr);
		int b = 8;
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
	case WM_NCLBUTTONDBLCLK:
		if (wp == HTCAPTION) { video_toggle_fullscreen(); return 0; }
		break;
	case WM_LBUTTONDBLCLK:
		video_toggle_fullscreen();
		return 0;
	case WM_GETMINMAXINFO: {
		MINMAXINFO *mm = (MINMAXINFO*)lp;
		mm->ptMinTrackSize.x = 160;
		mm->ptMinTrackSize.y = 120;
		return 0;
	}
	case WM_KEYDOWN:
		if (wp == 'F') { video_toggle_fullscreen(); return 0; }
		if (wp == VK_RETURN) { video_toggle_fullscreen(); return 0; }
		if (wp == VK_ESCAPE) {
			if (video.fullscreen) video_toggle_fullscreen();
			else { video.hidden = TRUE; video.win_visible = FALSE; ShowWindow(hwnd, SW_HIDE); }
			return 0;
		}
		break;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		if (!d3dev) { // GDI paint only when D3D is unavailable
			LARGE_INTEGER p0, p1;
			QueryPerformanceCounter(&p0);
			video_paint(dc);
			QueryPerformanceCounter(&p1);
			pr_stat(p0, p1);
		}
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_ACTIVATE:
		// owned-window z-order only guarantees we sit above XMPlay; keep the
		// owner visible right below us (without stealing focus)
		if (LOWORD(wp) != WA_INACTIVE && xmpwin)
			SetWindowPos(xmpwin, video.hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		break;
	case WM_ERASEBKGND:
		return 1;
	case WM_APP_VSHOW: {
		xmpwin = find_xmpwin(); // owner must be XMPlay itself
		if (xmpwin) SetWindowLong(video.hwnd, GWL_HWNDPARENT, (LONG)xmpwin);
		video.hidden = FALSE;
		if (!video.size_set) {
			SetWindowPos(video.hwnd, HWND_TOP, 0, 0, video.show_w, video.show_h,
				SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
			video.size_set = TRUE;
		}
		ShowWindow(video.hwnd, SW_SHOW);
		video.win_visible = TRUE;
		if (!d3dev) d3d_init(hwnd); // re-wake the device (powered down on hide)
		video_round_corners(hwnd);
		return 0;
	}
	case WM_APP_VHIDE:
		video.hidden = TRUE;
		video.win_visible = FALSE;
		ShowWindow(hwnd, SW_HIDE);
		if (!video.fullscreen) {
			// AUDIO-PRIORITY POWER DOWN: the user hid the picture (ESC) - the
			// decode thread is already idling, and now the D3D device + its
			// driver/DWM registration go away too, so the audio path returns
			// to pure-audio-track behavior until the next show.
			d3d_release();
			logmsg("dv: video hidden -> D3D device released (audio-priority idle)");
		}
		return 0;
	case WM_APP_VBLANK:
		video_blank();
		return 0;
	case WM_APP_VPRESENT:
		// diagnostic present throttle (VideoMode 4/5): cap how often the
		// picture is presented - isolates the PRESENT RATE from the audio
		if (video.present_throttle_ms &&
			(GetTickCount() - video.last_present_ms) < (DWORD)video.present_throttle_ms)
			return 0;
		video.last_present_ms = GetTickCount();
		if (d3dev) { // GPU blit - no GDI lock, audio stays smooth
			LARGE_INTEGER p0, p1;
			QueryPerformanceCounter(&p0);
			d3d_present();
			QueryPerformanceCounter(&p1);
			pr_stat(p0, p1);
		}
		else InvalidateRect(hwnd, 0, FALSE); // GDI fallback: measured in WM_PAINT
		return 0;
	case WM_SIZE:
		InvalidateRect(hwnd, 0, FALSE);
		return 0;
	}
	return DefWindowProc(hwnd, msg, wp, lp);
}

// the window thread: owns the video window + its message loop (and the GDI
// paint that comes with it). Runs below the audio so the window's own work
// never delays the audio pump either.
static DWORD WINAPI video_wnd_thread(LPVOID)
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
	QueryPerformanceFrequency(&g_prqf);
	WNDCLASSEX wc;
	memset(&wc, 0, sizeof wc);
	wc.cbSize = sizeof wc;
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = video_wndproc;
	wc.hInstance = dllinst;
	wc.hCursor = LoadCursor(0, (LPCSTR)IDC_ARROW);
	wc.lpszClassName = "XMPFFMPEG_VIDEO";
	if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return 1;
	RECT wr = { 0, 0, 640, 480 };
	DWORD style = WS_POPUP | WS_THICKFRAME; // thickframe keeps HT* resize; NCCALCSIZE drops the frame
	AdjustWindowRectEx(&wr, style, FALSE, 0);
	HWND h = CreateWindowEx(0, wc.lpszClassName, "FFmpeg video", style,
		CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, 0, 0, wc.hInstance, 0);
	if (!h) return 1;
	video.hwnd = h;
	d3d_init(h); // prefer GPU rendering; on failure we fall back to GDI paint
	logmsg("cb: window render=%s", d3dev ? "d3d" : "gdi");
	if (video.active)
		PostMessage(h, WM_APP_VSHOW, 0, 0); // a track is attached but our VSHOW
	                                          // may have been lost before we existed
	MSG m;
	while (GetMessage(&m, 0, 0, 0) > 0) {
		TranslateMessage(&m);
		DispatchMessage(&m);
	}
	return 0;
}

static DWORD WINAPI video_thread(LPVOID)
{
	// Decode thread: keep the latest frame decoded into video.dib. The picture
	// is shown in the plugin's OWN window (video_wnd_thread), not the XMPlay
	// vis - so the GDI blit is off XMPlay's threads and can't choke the audio.
	// Run at the LOWEST foreground priority so the decode bursts (h264 + sws)
	// always yield to audio work; the deep video queue absorbs dropped ticks.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
	while (1) {
		// window hidden / not shown: no one is looking at the picture - skip the
		// decode+scale work entirely and idle (zero CPU + zero pressure on audio).
		if (!video.win_visible && !video.seeking) {
			Sleep(250);
			continue;
		}
		video_tick();
		// tick cadence: during a seek catch-up run at ~1kHz so the queued stale
		// frames (keyframe -> target) are dropped at decoder speed (a local seek
		// shows the target in ~0.1-0.3s instead of ~2s). In normal playback tick
		// at the VIDEO'S OWN frame rate, not a fixed 66Hz: we never poll faster
		// than the stream has new frames, and fewer periodic wakeups means less
		// scheduler churn on the audio pump.
		DWORD sl;
		if (video.seeking) sl = 1;
		else {
			int fd = (int)(video.frame_dur * 1000.0 + 0.5); // ms per frame
			if (fd < 15) fd = 15;    // cap the poll at ~66fps
			if (fd > 100) fd = 100;  // safety floor on the tick interval
			sl = (DWORD)fd;
		}
		Sleep(sl);
	}
	return 0;
}

static void video_window_init()
{
	if (video.running) return;
	video.clock_base = 0;
	video.clock_tick = GetTickCount();
	if (g_video_mode == 1) return; // bisection: video attached, but no decode thread
	video.thread = CreateThread(0, 0, video_thread, 0, 0, &video.threadid);
	// the window thread owns the borderless video window + its message loop
	// (and the GDI paint). It is what keeps the blit off XMPlay's threads.
	video.wnd_thread = CreateThread(0, 0, video_wnd_thread, 0, 0, &video.wnd_threadid);
	video.running = TRUE;
}

// called on attach: size the window to the native video (clamped to the work
// area) and show it, docked to XMPlay's main window. Runs on the audio thread;
// the actual window ops are posted to the window thread.
static void video_show(FFMPEG *f)
{
	video_window_init();
	int w = f->vw > 0 ? f->vw : 640, h = f->vh > 0 ? f->vh : 480;
	RECT wa;
	if (SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0)) {
		int maxw = (int)((wa.right - wa.left) * 0.85), maxh = (int)((wa.bottom - wa.top) * 0.85);
		while (w > maxw || h > maxh) { w = w * 9 / 10; h = h * 9 / 10; }
	}
	video.show_w = w; video.show_h = h;
	PostMessage(video.hwnd, WM_APP_VSHOW, 0, 0); // no-op until the window thread created it
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
	// diagnostic present throttle (VideoMode 4 = ~1fps, 5 = once)
	video.present_throttle_ms = (g_video_mode == 4) ? 1000 :
		(g_video_mode == 5) ? 0x7FFFFFFF : 0;
	video.last_present_ms = 0;
	video.active = TRUE;
	LeaveCriticalSection(&video.lock);
	video_window_init(); // ensure the decode + window threads are alive
	video_show(f);      // show the video window at its native size, docked to XMPlay
}

// hide + disengage (Close / audio-only / teardown)
static void video_detach()
{
	// stop the render thread first (it checks video.active every tick), so no
	// new packets are enqueued after we clear the queue below.
	EnterCriticalSection(&video.lock);
	video.active = FALSE;
	video.have_frame = FALSE;
	video.vdec = 0; video.fmt = 0;
	video.have_new = FALSE;
	video.vis_blank = FALSE;
	video.vis_open = FALSE; // the decode thread idles until the vis reopens
	video.seeking = FALSE; // stop the fast catch-up cadence
	LeaveCriticalSection(&video.lock);
	// hide + clear the video window (posted to the window thread; a no-op if it
	// was never created). The picture must not survive a stop/track switch.
	video.win_visible = FALSE;
	if (video.hwnd) {
		PostMessage(video.hwnd, WM_APP_VBLANK, 0, 0);
		PostMessage(video.hwnd, WM_APP_VHIDE, 0, 0);
	}
	EnterCriticalSection(&video.qlock);
	VPKT *p = video.qhead;
	while (p) { VPKT *n = p->next; av_packet_free(&p->pkt); free(p); p = n; }
	video.qhead = video.qtail = 0; video.qcount = 0;
	LeaveCriticalSection(&video.qlock);
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

// ------------------------------------------------- direct ID3v2 tag reader
// FFmpeg's custom-IO (XMPFILE) open path does not surface ID3v2 tags into
// fmt->metadata for MP3, so when the demuxer leaves them empty we parse the
// file's own TIT2/TPE1 frames directly. Handles ID3v2.3 and 2.4 (4-char
// frames) and the four text encodings; values are returned as UTF-8.
static void id3_text_to_utf8(const unsigned char *p, int n, unsigned char enc,
                             char *out, int outn)
{
	if (outn <= 0) return;
	out[0] = 0;
	wchar_t w[512];
	int wl = 0;
	switch (enc) {
	case 0: // 8-bit: ISO-8859-1 per spec, but CJK tools store CP_ACP bytes
		wl = MultiByteToWideChar(CP_ACP, 0, (const char*)p, n, w, 511);
		break;
	case 3: // UTF-8
		wl = MultiByteToWideChar(CP_UTF8, 0, (const char*)p, n, w, 511);
		break;
	case 1: // UTF-16, BOM decides endianness, native (LE) when no BOM
	case 2: { // UTF-16BE
		int i = 0, be = (enc == 2);
		if (enc == 1) {
			if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { i = 2; be = 0; }
			else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { i = 2; be = 1; }
		}
		int pairs = (n - i) / 2;
		if (pairs > 511) pairs = 511;
		for (int k = 0; k < pairs; k++)
			w[k] = be ? (wchar_t)((p[i + 2 * k] << 8) | p[i + 2 * k + 1])
			         : (wchar_t)((p[i + 2 * k + 1] << 8) | p[i + 2 * k]);
		wl = pairs;
		break;
	}
	}
	if (wl < 0) wl = 0;
	while (wl > 0 && w[wl - 1] == 0) wl--;
	int need = WideCharToMultiByte(CP_UTF8, 0, w, wl, 0, 0, 0, 0);
	if (need >= outn) need = outn - 1;
	if (need < 1) { out[0] = 0; return; }
	WideCharToMultiByte(CP_UTF8, 0, w, wl, out, need, 0, 0);
	out[need] = 0;
}
static void read_id3v2(const char *filename, char *artist, int an, char *title, int tn)
{
	wchar_t *wp = utf8_path_to_wide(filename);
	FILE *fp = wp ? _wfopen(wp, L"rb") : 0;
	free(wp);
	if (!fp) return;
	unsigned char hdr[10];
	if (fread(hdr, 1, 10, fp) != 10) { fclose(fp); return; }
	if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3' || (hdr[3] != 3 && hdr[3] != 4)) {
		fclose(fp); return; // only v2.3/v2.4 (4-char frames) are supported
	}
	int ver = hdr[3];
	int taglen = ((int)hdr[6] << 21) | ((int)hdr[7] << 14) |
	             ((int)hdr[8] << 7) | (int)hdr[9];
	taglen += 10;                       // absolute offset just past the tag
	int gt = 0, ga = 0;               // got title / got artist
	unsigned char body[1024];
	while (!gt || !ga) {
		long here = ftell(fp);
		if (here + 10 > taglen) break;
		unsigned char fh[10];
		if (fread(fh, 1, 10, fp) != 10) break;
		if (fh[0] == 0) break;        // reached the padding block
		char id[5];
		id[0] = fh[0]; id[1] = fh[1]; id[2] = fh[2]; id[3] = fh[3]; id[4] = 0;
		int is_title = id[0] == 'T' && id[1] == 'I' && id[2] == 'T' &&
		               (id[3] == '2' || id[3] == '3');
		int is_art = id[0] == 'T' && id[1] == 'P' && id[2] == 'E' && id[3] == '1';
		int sz = (ver == 4) ?
			(((int)fh[4] << 21) | ((int)fh[5] << 14) | ((int)fh[6] << 7) | (int)fh[7]) :
			(((int)fh[4] << 24) | ((int)fh[5] << 16) | ((int)fh[6] << 8) | (int)fh[7]);
		if (sz <= 0 || sz > 100000) break;          // malformed size: stop
		int rl = sz < (int)sizeof body ? sz : (int)sizeof body;
		int rd = rl ? (int)fread(body, 1, rl, fp) : 0;
		if ((is_title || is_art) && rd >= 2) {
			char txt[1024];
			id3_text_to_utf8(body + 1, rd - 1, body[0], txt, sizeof txt);
			if (is_title && !gt && txt[0]) { bcpy(title, tn, txt); gt = 1; }
			if (is_art && !ga && txt[0]) { bcpy(artist, an, txt); ga = 1; }
		}
		if (sz - rd > 0 && fseek(fp, sz - rd, SEEK_CUR) != 0) break;
	}
	fclose(fp);
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
// graduated font cache: HFONTs keyed by point size, rebuilt only when the
// font name changes. The dynamic-size lyric animation picks the nearest size
// per line instead of recreating GDI fonts every frame.
static HFONT lrc_fonts[96];
static char lrc_font_sig[64];
static int lrc_font_sig_size = -1;

static HFONT lrc_font_pt(int pt)
{
	if (pt < 4) pt = 4;
	if (pt > 95) pt = 95;
	int len = (int)strlen(lrc_cfg.font_name);
	int same = (lrc_font_sig_size == len);
	if (same) {
		int cmp = len < (int)sizeof lrc_font_sig ? len : (int)sizeof lrc_font_sig - 1;
		for (int i = 0; i < cmp; i++) if (lrc_font_sig[i] != lrc_cfg.font_name[i]) { same = 0; break; }
	}
	if (!same) {
		for (int i = 0; i < 96; i++) if (lrc_fonts[i]) { DeleteObject(lrc_fonts[i]); lrc_fonts[i] = 0; }
		bcpy(lrc_font_sig, sizeof lrc_font_sig, lrc_cfg.font_name);
		lrc_font_sig_size = len;
	}
	if (!lrc_fonts[pt]) {
		wchar_t fn[64];
		MultiByteToWideChar(CP_UTF8, 0, lrc_cfg.font_name, -1, fn, 64);
		lrc_fonts[pt] = CreateFontW(-pt, 0, 0, 0, FW_NORMAL, FALSE, FALSE, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
			DEFAULT_PITCH | FF_DONTCARE, fn);
		if (!lrc_fonts[pt])
			lrc_fonts[pt] = CreateFontW(-pt, 0, 0, 0, FW_NORMAL, 0, 0, 0,
				DEFAULT_CHARSET, 0, 0, DEFAULT_QUALITY, DEFAULT_PITCH, L"Tahoma");
	}
	return lrc_fonts[pt];
}

static int lyric_render(HDC dc, int W, int H)
{
	// returns 1 if lyrics were drawn, 0 otherwise (caller falls back to black)
	// effective time = playback clock - user offset
	double t = video_clock_now() - (double)lrc_cfg.offset_tenths * 0.1;
	int lh = lrc_cfg.font_size + 6;      // px the scroll travels per line
	int cy = H / 2;                      // focal line anchored mid-panel

	double fp = 0;                       // focal position in line-space
	int cur = -1;                        // current line index (the large one)
	const char *texts[48];
	int idxs[48];
	int n = 0;
	{
		EnterCriticalSection(&lyric.lock);
		if (lyric.state == 2 && lyric.nlines > 0) {
			for (int i = 0; i < lyric.nlines; i++)
				if (t >= lyric.lines[i].t) cur = i;
			if (cur < 0) cur = 0;
			double f = 0;
			if (cur + 1 < lyric.nlines) {  // progress 0->1 toward the next line
				double t0 = lyric.lines[cur].t, t1 = lyric.lines[cur + 1].t;
				double span = t1 - t0;
				f = (span > 0.0) ? (t - t0) / span : 1.0;
				if (f < 0) f = 0;
				if (f > 1) f = 1;
			}
			fp = (double)cur + f;          // last line rests centered (f stays 0)
			int spanLines = (H / lh) + 2;
			int a = (int)fp - spanLines;
			int b = (int)fp + spanLines;
			for (int i = 0; i < lyric.nlines; i++) {
				if (i < a || i > b) continue;
				texts[n] = lyric.lines[i].text;
				idxs[n] = i;
				if (++n >= 48) break;
			}
		}
		LeaveCriticalSection(&lyric.lock);
	}
	if (n == 0) return 0;

	int bg = (int)vis_colors[0];
	int txt = (int)vis_colors[1];
	int R = (bg >> 16) & 0xff, G = (bg >> 8) & 0xff, B = bg & 0xff;
	int tr = (txt >> 16) & 0xff, tg = (txt >> 8) & 0xff, tb = txt & 0xff;
	int dr = R + (tr - R) / 2, dg = G + (tg - G) / 2, db = B + (tb - B) / 2; // dim base
	int bigpt = (int)(lrc_cfg.font_size * 1.5 + 0.5); // the current line, larger

	HFONT oldf = (HFONT)SelectObject(dc, lrc_font_pt(lrc_cfg.font_size));
	HPEN oldpen = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
	HGDIOBJ oldbr = SelectObject(dc, GetStockObject(NULL_BRUSH));
	SetBkMode(dc, TRANSPARENT);
	for (int k = 0; k < n; k++) {
		int iscur = (idxs[k] == cur);
		double dist = (double)idxs[k] - fp;
		int y = cy + (int)(dist * lh + 0.5);         // smooth sub-line scroll
		wchar_t wtext[512];
		MultiByteToWideChar(CP_UTF8, 0, texts[k], -1, wtext, 512);
		RECT rc = { 0, y - lh / 2, W, y + lh / 2 };
		SelectObject(dc, lrc_font_pt(iscur ? bigpt : lrc_cfg.font_size));
		SetTextColor(dc, iscur ? (COLORREF)lrc_cfg.cur_color : RGB(dr, dg, db));
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
	video.vis_open = TRUE;  // decode thread resumes (was idling when closed)
	LeaveCriticalSection(&video.lock);
	return TRUE;
}

static void WINAPI FF_VisClose()
{
	logmsg("vis: close");
	EnterCriticalSection(&video.lock);
	video.vis_open = FALSE; // decode thread idles - no CPU while the vis is hidden
	LeaveCriticalSection(&video.lock);
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

	// playing: decide what the vis area shows.
	EnterCriticalSection(&video.lock);
	int is_video = (video.vdec != 0);
	LeaveCriticalSection(&video.lock);

	if (is_video) {
		// video track: the picture lives in the plugin's OWN window, not this
		// vis area - so the vis just goes black and stops refreshing. No GDI
		// video blit happens here: that per-frame GDI on XMPlay's thread is
		// exactly what chokes the audio (the mode-3 bisection).
		EnterCriticalSection(&video.lock);
		BOOL need_blank = !video.vis_blank;
		if (need_blank) video.vis_blank = TRUE;
		LeaveCriticalSection(&video.lock);
		if (need_blank) {
			FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
			return TRUE; // cleared once; XMPlay stops refreshing a static display
		}
		return FALSE;
	}

	// audio-only track: the vis shows the skin background + LRC lyrics,
	// redrawn each refresh until the lyrics change (fetching is backgrounded)
	HBRUSH bg = CreateSolidBrush((COLORREF)vis_colors[0]);
	FillRect(dc, &rc, bg);
	DeleteObject(bg);
	lyric_render(dc, size.cx, size.cy); // draws nothing until lyrics are ready
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
	// read the bisection switch each open so it can be flipped without a rebuild
	{ int vm = 0;
	  if (xmpfreg && xmpfreg->GetInt(LRC_REGSEC, "VideoMode", &vm))
		  g_video_mode = (vm >= 0 && vm <= 5) ? vm : 0;
	  else g_video_mode = 0;
	  logmsg("cb: VideoMode=%d", g_video_mode); }
	if (g_video_mode != 2)
		video_attach(current); // attach the vis video iff this file has a video stream
	if (current->vstream < 0 || g_video_mode == 2) {
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
		// source 2: when the demuxer left the tags empty, read the file's own
		// ID3v2 TIT2/TPE1 frames directly (FFmpeg's custom-IO open does not
		// surface ID3v2 metadata for MP3). Only fills what is still missing.
		if (is_local && filename && (!artist[0] || !title[0])) {
			char a2[512] = "", t2[512] = "";
			read_id3v2(filename, a2, sizeof a2, t2, sizeof t2);
			if (!artist[0] && a2[0]) bcpy(artist, sizeof artist, a2);
			if (!title[0] && t2[0]) bcpy(title, sizeof title, t2);
		}
		// Backfill missing tags from the "Artist - Title" file name (XMPlay's
		// default TitleForm). Fires when the tags left a title or an artist
		// empty; a clean split beats the whole-stem title the service can't
		// match (its search is title-containment, so "Artist - Title" returns
		// nothing while "Title" + "Artist" does). Tags always win when set.
		if (is_local && filename && (!title[0] || !artist[0])) {
			char stem[512];
			const char *base = filename;
			for (const char *q = filename; *q; q++)
				if (*q == '\\' || *q == '/') base = q + 1;
			bcpy(stem, sizeof stem, base);
			char *dot = stem + strlen(stem);
			while (dot > stem && *dot != '.') dot--;
			if (dot > stem) *dot = 0;            // drop the extension
			const char *sep = strstr(stem, " - ");
			if (!sep) sep = strstr(stem, " _ ");
			char a[512] = "", t2[512];
			if (sep) {
				size_t al = (size_t)(sep - stem);
				memcpy(a, stem, al); a[al] = 0;  // left  = artist
				bcpy(t2, sizeof t2, sep + 3);    // right = title
			} else {
				bcpy(t2, sizeof t2, stem);        // no separator: whole stem
			}
			char *pa = a, *pt = t2;   // trim via pointers (arrays can't ++ )
			while (*pa == ' ' || *pa == '\t') pa++;
			while (*pt == ' ' || *pt == '\t') pt++;
			if (!artist[0] && pa[0]) bcpy(artist, sizeof artist, pa);
			if (!title[0]) bcpy(title, sizeof title, pt[0] ? pt : stem);
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
	if (g_video_mode == 2) return; // forced audio-only: never attach video
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
		vdec->thread_count = 1; // single-threaded decode (see the main open path)
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
	DWORD c0 = GetTickCount(); // whole-call + call-cadence timing (pb: diagnostics)
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
	{
		// audio-pump diagnostics: call cadence (gap vs the expected interval for
		// `count` samples), worst whole-call time in the window, underruns.
		// gap >> exp means XMPlay is calling the pump late -> the sound-card
		// buffer drains -> the "chop". gap ~ exp + a steady device buffer = fine.
		static DWORD pb_last = 0;
		static DWORD pb_lastcall = 0;
		static DWORD pb_worst = 0;
		static unsigned long pb_ur = 0;
		DWORD now = GetTickCount();
		DWORD gap = pb_lastcall ? (c0 - pb_lastcall) : 0;
		pb_lastcall = c0;
		DWORD exp = (DWORD)(((double)count * 1000.0) /
			(f->dec->sample_rate ? f->dec->sample_rate : 44100));
		DWORD tot = now - c0;
		if (tot > pb_worst) pb_worst = tot;
		if (done < count) pb_ur++;
		if (now - pb_last >= 200) {
			logmsg("pb: gap=%lu exp=%lu tot=%lu ur=%lu fpos=%.2f",
				(unsigned long)gap, (unsigned long)exp,
				(unsigned long)pb_worst, pb_ur, f->pos);
			pb_last = now;
			pb_worst = 0;
			pb_ur = 0;
		}
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
	InitializeCriticalSection(&g_loglock); // log handle + guard (cheap logmsg)
	InitializeCriticalSection(&video.lock);
	InitializeCriticalSection(&video.qlock);
	InitializeCriticalSection(&video.clocklock);
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

// ------------------------------------------------------- high-resolution timer
// The audio mixer's thin buffer turns ANY system DPC / timer-wakeup jitter
// into audible chop on video tracks (the video path keeps the CPU + the
// display pipeline busy). Requesting the 1ms timer period while the plugin
// is loaded keeps the system's short sleeps and timer DPCs precise instead
// of letting them coalesce at 15.6ms (the classic Win32 default) - the
// standard mitigation for this class of audio glitch. winmm.dll is loaded
// DYNAMICALLY (never in the import table) so the XP-compat audit stays
// clean; on XP winmm.dll + timeBeginPeriod have always existed.
static HMODULE g_winmm = 0;
static BOOL (WINAPI *g_time_begin_period)(unsigned) = 0;
static BOOL (WINAPI *g_time_end_period)(unsigned) = 0;

static void timer_period_begin(void)
{
	g_winmm = LoadLibraryA("winmm.dll");
	if (!g_winmm) return;
	g_time_begin_period = (BOOL (WINAPI *)(unsigned))
		GetProcAddress(g_winmm, "timeBeginPeriod");
	g_time_end_period = (BOOL (WINAPI *)(unsigned))
		GetProcAddress(g_winmm, "timeEndPeriod");
	if (g_time_begin_period) g_time_begin_period(1);
}

static void timer_period_end(void)
{
	if (g_time_end_period) g_time_end_period(1);
	if (g_winmm) { FreeLibrary(g_winmm); g_winmm = 0; }
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
		timer_period_begin(); // 1ms timer period: precise short sleeps for the
		                      // audio/video paths (freed in PROCESS_DETACH)
		break;
	case DLL_PROCESS_DETACH:
		timer_period_end();
		if (ws_init_done) WSACleanup();
		if (lyric_fetch_handle) CloseHandle(lyric_fetch_handle);
		DeleteCriticalSection(&lyric_thread_lock);
		DeleteCriticalSection(&lyric.lock);
		break;
	}
	return TRUE;
}
#endif
