#!/usr/bin/env bash
# build-sdl2.sh - build a static SDL2 (video + render only) for the cross
# toolchain, so the video window can render through SDL2's D3D9-backed
# renderer instead of hand-rolled D3D (which crashes this machine's Intel
# 32-bit D3D UMD). SDL2 is linked statically into xmp-ffmpeg.dll - the
# plugin stays a single file, no SDL2.dll to ship.
#
# Kept as its own script (NOT merged into build-ffmpeg.sh) so the FFmpeg /
# toolchain rolling cache key stays untouched; the SDL2 archive lands in the
# same sandbox dir and is cached alongside the toolchain.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT=$PWD
SANDBOX=$ROOT/sandbox
CROSS=$SANDBOX/cross_compilers/mingw-w64-i686
TRIPLE=i686-w64-mingw32
PREFIX=$CROSS/$TRIPLE
SRC=$SANDBOX/src
JOBS=${JOBS:-$(nproc)}
SDLVER=2.0.18
CFLAGS_XP="-O2 -march=pentium3 -mtune=athlon-xp -mfpmath=sse -msse"

export PATH=$CROSS/bin:$PATH

if [[ ! -f $PREFIX/lib/libSDL2.a ]]; then
	[[ -f $SRC/SDL2-$SDLVER.tar.gz ]] || \
		curl -sL --retry 3 --connect-timeout 20 -o "$SRC/SDL2-$SDLVER.tar.gz" \
			"https://github.com/libsdl-org/SDL/releases/download/release-$SDLVER/SDL2-$SDLVER.tar.gz"
	tar -C "$SRC" -xf "$SRC/SDL2-$SDLVER.tar.gz"
	(
		cd "$SRC/SDL2-$SDLVER"
		# video + render only: audio/joystick/haptic off (we never use them),
		# static, XP-target CFLAGS matching the FFmpeg build. SDL 2.0.18 is
		# XP-compatible (SDL3 is the one that dropped XP).
		env -u CFLAGS -u CXXFLAGS -u LDFLAGS \
			CC="$TRIPLE-gcc" \
			./configure --host=$TRIPLE --prefix="$PREFIX" \
				--enable-static --disable-shared \
				--disable-audio --disable-joystick --disable-haptic \
				--disable-hidapi --disable-pulse \
				--disable-x11 --disable-cocoa --disable-fusion --disable-wayland \
				--disable-video-caca --disable-video-rpidma --disable-video-vivante \
				CFLAGS="$CFLAGS_XP" LDFLAGS=""
		make -j$JOBS
		make install
	)
fi

echo "== SDL2 ready: $PREFIX/lib/libSDL2.a =="
ls -la "$PREFIX/lib/libSDL2.a"
# headers must be where build-plugin.sh looks (-I$PREFIX/include)
test -f "$PREFIX/include/SDL2/SDL.h"
