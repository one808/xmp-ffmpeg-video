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
};
static VIDEO video;

static void video_enqueue(AVPacket *pkt); // takes ownership
static void video_flush();
static void video_attach(FFMPEG *f);
static void video_detach();

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
	if (f->io) {
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

	const int IOBUF = 64 * 1024;
	unsigned char *iobuf = (unsigned char*)av_malloc(IOBUF);
	f->io = avio_alloc_context(iobuf, IOBUF, 0, f, io_read, NULL, f->seekable ? io_seek : NULL);
	if (!f->io) { av_free(iobuf); free(f); return 0; }
	f->io->seekable = f->seekable ? AVIO_SEEKABLE_NORMAL : 0;

	f->fmt = avformat_alloc_context();
	if (!f->fmt) goto fail;
	f->fmt->pb = f->io;
	f->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
	if (avformat_open_input(&f->fmt, NULL, NULL, NULL) < 0) goto fail;
	if (avformat_find_stream_info(f->fmt, NULL) < 0) goto fail;

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
				} else if (f->vdec) {
					avcodec_free_context(&f->vdec);
				}
			}
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
		if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
			f->duration = st->duration * av_q2d(st->time_base);
		else if (f->fmt->duration != AV_NOPTS_VALUE && f->fmt->duration > 0)
			f->duration = (double)f->fmt->duration / AV_TIME_BASE;
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
	if (video.vdec) avcodec_flush_buffers(video.vdec);
	LeaveCriticalSection(&video.lock);
}

// -- playback clock (captured on XMPlay's thread, interpolated on ours)

static void video_clock_capture(double t)
{
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
	if (cw < 1 || ch < 1 || !video.dib || video.dibw < 1) return;
	int w, h, x, y;
	double va = (double)video.dibw / video.dibh, wa = (double)cw / ch;
	if (wa > va) { h = ch; w = (int)(h * va + 0.5); x = (cw - w) / 2; y = 0; }
	else { w = cw; h = (int)(w / va + 0.5); x = 0; y = (ch - h) / 2; }
	StretchDIBits(dc, x, y, w, h, 0, 0, video.dibw, video.dibh,
		video.dib, &video.bmi, DIB_RGB_COLORS, SRCCOPY);
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

// -- sync tick: decode/present frames due at XMPlay's playback position

static void video_tick()
{
	if (!video.active || video.hidden || !video.vdec) return;
	double t = video_clock_now(); // interpolated playback clock
	EnterCriticalSection(&video.lock);
	if (!video.active) { LeaveCriticalSection(&video.lock); return; }
	for (;;) {
		if (!video.have_frame) {
			int ret = avcodec_receive_frame(video.vdec, video.vframe);
			if (ret == AVERROR(EAGAIN)) {
				// feed one packet (or the drain marker at demux EOF)
				if (video.qhead) {
					VPKT *n = video.qhead;
					video.qhead = n->next;
					if (!video.qhead) video.qtail = 0;
					video.qcount--;
					avcodec_send_packet(video.vdec, n->pkt);
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
			double pts = t; // fall back to the clock when no stamp
			if (video.vframe->best_effort_timestamp != AV_NOPTS_VALUE)
				pts = video.vframe->best_effort_timestamp * av_q2d(video.tb);
			video.frame_pts = pts;
			video.have_frame = TRUE;
			continue;
		}
		// have a decoded frame: show it when due, drop when far too late
		if (video.frame_pts > t + 0.06) break;   // in the future - wait
		if (video.frame_pts < t - 0.30) {        // way behind - drop
			video.have_frame = FALSE;
			continue;
		}
		video_render_frame();
		video.have_frame = FALSE;
		break; // one present per tick is enough
	}
	LeaveCriticalSection(&video.lock);
}

// -- window

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
	case WM_NCHITTEST: // borderless: drag by the client area (resize via frame)
		if (!video.fullscreen) {
			LRESULT r = DefWindowProc(hwnd, msg, wp, lp);
			return (r == HTCLIENT) ? HTCAPTION : r;
		}
		break;
	case WM_LBUTTONDBLCLK:
		video_toggle_fullscreen();
		return 0;
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
		RECT rc; GetClientRect(hwnd, &rc);
		FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
		if (video.dib) video_paint(dc);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_APP_SHOW: {
		SetWindowPos(video.hwnd, HWND_TOP, 0, 0, video.show_w, video.show_h,
			SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
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
		0, 0, wc.hInstance, 0);
	SetTimer(video.hwnd, 1, 15, 0);
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

static BOOL WINAPI FF_CheckFile(const char *filename, XMPFILE file)
{
	return TRUE;
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
	video_clock_capture(0);
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
	form->rate = f->dec->sample_rate;
	form->chan = f->dec->ch_layout.nb_channels;
	form->res = res_code(f->dec->sample_fmt, f->dec->bits_per_raw_sample);
	form->chanmask = (f->dec->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) ? (WORD)f->dec->ch_layout.u.mask : 0;
}

static void WINAPI FF_GetInfoText(char *format, char *length)
{
	FFMPEG *f = current;
	if (format) {
		format += sprintf(format, "%s", f->codec ? f->codec : "?");
		if (f->bitrate) format += sprintf(format, " - %ukb/s", (unsigned)f->bitrate);
		sprintf(format, " - %dhz", f->dec->sample_rate);
	}
}

static void WINAPI FF_GetGeneralInfo(char *buf)
{
	FFMPEG *f = current;
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
	AVDictionary *md[2] = { f->fmt->streams[f->stream]->metadata, f->fmt->metadata };
	for (int i = 0; i < 2; i++) {
		AVDictionaryEntry *e = NULL;
		while ((e = av_dict_get(md[i], "", e, AV_DICT_IGNORE_SUFFIX)))
			buf = xmpfmisc->FormatInfoText(buf, e->key, e->value);
	}
}

static DWORD WINAPI FF_Process(float *buffer, DWORD count)
{
	FFMPEG *f = current;
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
	double t = pos * 0.001;
	if (!f->seekable) return -1;
	if (av_seek_frame(f->fmt, -1, (int64_t)(t * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) < 0) return -1;
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
		XMPIN_FLAG_CANSTREAM | XMPIN_FLAG_NOCHECK | XMPIN_FLAG_MULTIEXT,
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
	InitializeCriticalSection(&video.lock);
	video.ready = 0;
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
