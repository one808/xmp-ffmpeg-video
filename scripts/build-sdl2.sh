#!/usr/bin/env bash
# build-sdl2.sh - stage a static SDL2 (video + render) for the plugin link.
#
# Uses SDL's OFFICIAL MinGW package (SDL2-devel-<ver>-mingw.tar.gz): a
# prebuilt i686 libSDL2.a + headers. No compile step, no version drift.
# The archive is staged into the shared prefix so build-plugin.sh links
# $PREFIX/lib/libSDL2.a and compiles against $PREFIX/include/SDL2.
#
# (Fallback if the prebuilt .a ever fails to link against our toolchain:
# build from the source archive with the cross toolchain - the URL for that
# is /libsdl-org/SDL/archive/refs/tags/release-<ver>.tar.gz.)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT=$PWD
SANDBOX=$ROOT/sandbox
CROSS=$SANDBOX/cross_compilers/mingw-w64-i686
PREFIX=$CROSS/i686-w64-mingw32
SRC=$SANDBOX/src
SDLVER=2.0.18
URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDLVER/SDL2-devel-$SDLVER-mingw.tar.gz"

export PATH=$CROSS/bin:$PATH

if [[ ! -f $PREFIX/lib/libSDL2.a ]]; then
	mkdir -p "$SRC"
	TARBALL="$SRC/SDL2-devel-$SDLVER-mingw.tar.gz"
	# -f: fail (and remove the partial file) on HTTP errors like 404
	[[ -f $TARBALL ]] || curl -fL --retry 3 --connect-timeout 20 -o "$TARBALL" "$URL"
	# guard: a broken download is not gzip
	head -c2 "$TARBALL" | od -An -tx1 | grep -q "1f 8b" || \
		{ echo "ERROR: SDL2 tarball is not gzip (download failed?)" >&2; exit 1; }
	rm -rf "$SRC/sdl2-mingw-extract"
	mkdir -p "$SRC/sdl2-mingw-extract"
	tar -C "$SRC/sdl2-mingw-extract" -xf "$TARBALL"
	DIR="$SRC/sdl2-mingw-extract"
	# the archive's top-level folder name varies - locate the 32-bit (i686)
	# static library + headers wherever they are (NOT the x86_64 ones).
	# NOTE: the dir is "i686-w64-mingw32" (dash, not slash) - match without
	# a trailing slash, and make the find|grep|head pipeline explicit so a
	# no-match is a clean error under `set -euo pipefail`.
	A=$(find "$DIR" \( -name "SDL2.a" -o -name "libSDL2.a" \) | grep -E "/i[56]86" || true)
	[[ -n $A ]] || { echo "ERROR: 32-bit SDL2 static lib not found in the mingw package" >&2; find "$DIR" -name "*.a" | sed 's/^/    /' >&2; exit 1; }
	H=$(find "$DIR" -name "SDL.h" -path "*/include/SDL2/SDL.h" | grep -E "/i[56]86" || true)
	[[ -n $H ]] || { echo "ERROR: 32-bit SDL.h not found in the mingw package" >&2; find "$DIR" -name "SDL.h" | sed 's/^/    /' >&2; exit 1; }
	cp "$A" "$PREFIX/lib/libSDL2.a"
	rm -rf "$PREFIX/include/SDL2"
	cp -r "$(dirname "$H")" "$PREFIX/include/SDL2"
fi

echo "== SDL2 ready: $PREFIX/lib/libSDL2.a =="
ls -la "$PREFIX/lib/libSDL2.a"
test -f "$PREFIX/include/SDL2/SDL.h"
