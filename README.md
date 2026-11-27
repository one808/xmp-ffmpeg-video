# xmp-ffmpeg-video

XMPlay input plugin that plays **anything FFmpeg can decode** — all audio
formats, plus the audio track of any video file — and opens a **borderless
video window** automatically when the file has a video stream. Powered by a
fully **static** FFmpeg build: zero FFmpeg DLLs.

This is the video-window variant of
[xmp-ffmpeg](https://github.com/one808/xmp-ffmpeg) (same engine, same build
pipeline). Use **this** DLL instead of the audio-only one — not alongside.

Built for **Windows XP and later** (including old CPUs without SSE2).

## The video window

- **Opens automatically** when the track has a video stream; **stays hidden**
  for audio-only tracks (and when you switch between the two).
- **Borderless**: drag anywhere to move, drag the edges to resize (the video
  keeps its aspect ratio, letterboxed).
- **Fullscreen**: double-click, or `F` / `Enter`. `ESC` leaves fullscreen —
  press `ESC` again to hide the window (playback continues).
- Sync: the picture follows XMPlay's playback position; late frames are
  dropped rather than delaying the audio.

## Features (audio side)

- **All formats**: 231+ audio decoders (incl. PCM family), 367 demuxers —
  MP3/AAC/M4A/FLAC/WAV/WMA/APE/WavPack/TTA/TAK/MPC/Opus/Vorbis/AC3/E-AC3/DTS/
  TrueHD/MLP/AMR/GSM/ATRAC/…, game music (via libgme), codec2, libfdk-aac.
- **Video files play their audio**: MKV/MP4/AVI/MOV/WMV/FLV/WebM/TS/M2TS/VOB/
  3GP/RM/… — and now with the picture, too.
- **Network streaming** through XMPlay's own file routines (HTTP/ICY, archives).
- Multichannel audio with speaker masks, millisecond seeking, full tags.
- Module formats (MOD/XM/S3M/IT…) are deliberately **not** claimed: XMPlay
  plays those natively, and better.

## Installation

Copy `xmp-ffmpeg.dll` into XMPlay's folder (or its `plugins` folder) and
restart XMPlay. If you previously had the audio-only `xmp-ffmpeg.dll`, this
one replaces it.

## Building

Everything is built with GitHub Actions (`.github/workflows/build.yml`) —
on every push, or manually via *Run workflow*. The pipeline is the same as
[xmp-ffmpeg](https://github.com/one808/xmp-ffmpeg): the proven XP toolchain
from [ffmpeg-windows-build-helpers](https://github.com/one808/ffmpeg-windows-build-helpers),
pinned external codec libs, FFmpeg `n9.0.2` decode-only **with video decoders
and swscale enabled**, XP patches, then a PE audit (subsystem ≤ 5.1, no
Vista+-only imports, no non-system DLL dependencies).

## Releases & updates

The version lives in the `VERSION` file (single source of truth) and is
embedded into the DLL as a standard Windows **VERSIONINFO resource**. Every
successful `main` build publishes an **add-only** GitHub Release (existing
releases are never modified): tag `v<version>`, or `v<version>.<run>` when
that version already has a release.

## Notes

- Audio: decoded at the source rate/layout (float32 internally); XMPlay
  resamples to the device.
- Video: decoded on a dedicated thread; GDI presentation (DIBSection +
  StretchDIBits) keeps it XP-trivial. Modern HD content needs a strong CPU —
  the realistic sweet spot on vintage hardware is SD content (MPEG-1/2,
  DivX, WMV, MPEG-4 ASP…).
- Video streams are never mixed into the audio path; the audio timeline is
  master and the picture follows.

## Credits

- [FFmpeg](https://ffmpeg.org) — decoding (LGPL/GPL, built GPL v3 here)
- [XMPlay plugin SDK](https://www.un4seen.com/) by un4seen.com
- [ffmpeg-windows-build-helpers](https://github.com/one808/ffmpeg-windows-build-helpers)
  — toolchain recipe, XP patches and audit tooling

## License

GPL v3 or later (FFmpeg is built with `--enable-gpl --enable-version3`).
See `LICENSE`. Note: libfdk-aac is linked statically (FDK license, hence
FFmpeg's `--enable-nonfree`); remove `--enable-libfdk-aac` from
`scripts/build-ffmpeg.sh` for a strictly GPL-pure build.
