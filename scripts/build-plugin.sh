#!/usr/bin/env bash
# build-plugin.sh - compile and link xmp-ffmpeg.dll (static, no FFmpeg DLLs).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
CROSS=$PWD/sandbox/cross_compilers/mingw-w64-i686
TRIPLE=i686-w64-mingw32
PREFIX=$CROSS/$TRIPLE
CFLAGS_XP="-O2 -march=pentium3 -mtune=athlon-xp -mfpmath=sse -msse"
DEFS_XP=""

export PATH=$CROSS/bin:$PATH
mkdir -p build

# single source of truth for the version: the VERSION file at the repo root
VER=$(tr -d ' \n' < VERSION)
VERNUM=$(echo "$VER.0.0.0" | cut -d. -f1-4 | tr '.' ',')
echo "== building xmp-ffmpeg $VER =="

echo "== generating version resource (VERSIONINFO) =="
sed -e "s/@VERSION@/$VER/g" -e "s/@VERSIONNUM@/$VERNUM/g" src/xmp-ffmpeg.rc > build/xmp-ffmpeg.rc
$TRIPLE-windres -O coff build/xmp-ffmpeg.rc build/xmp-ffmpeg-res.o

echo "== compiling xmp-ffmpeg.cpp =="
$TRIPLE-g++ $CFLAGS_XP $DEFS_XP -Os -fno-exceptions -fno-rtti \
	-DXMP_FFMPEG_VERSION=\"$VER\" \
	-Ixmp-sdk -I"$PREFIX/include" \
	-c src/xmp-ffmpeg.cpp -o build/xmp-ffmpeg.o

echo "== linking xmp-ffmpeg.dll (static) =="
# pkg-config resolves the FULL static dependency set (incl. Windows system libs
# like -ladvapi32 for the wincrypt fallback) from the FFmpeg/dep .pc files.
# Note: our toolchain is the win32 thread model + pthreads4w => the library is
# -lpthread (there is no -lwinpthread in this toolchain).
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR=
FFLIBS=$(pkg-config --static --libs libavformat libavcodec libswresample libswscale libavutil)
echo "link libs: $FFLIBS"
# g++ driver + -static-libstdc++/-static-libgcc: the dep .pc files pull in
# -lstdc++ (gme is C++), and we must NOT end up importing libstdc++-6.dll /
# libgcc_s_*.dll - everything goes into this single DLL.
$TRIPLE-g++ -shared -o build/xmp-ffmpeg.dll build/xmp-ffmpeg.o build/xmp-ffmpeg-res.o \
	-Wl,--kill-at -static-libstdc++ -static-libgcc $FFLIBS \
	-lssp -latomic -lgdi32 -luser32

# sanity: the VERSIONINFO resource must be embedded (XMPlay reads it).
# Binary UTF-16-aware check - `strings` misses short versions like "1.1"
# and cannot see UTF-16 resource strings at all.
python3 - "$VER" build/xmp-ffmpeg.dll << 'PYCHECK'
import sys
ver, path = sys.argv[1], sys.argv[2]
d = open(path, 'rb').read()
u16 = lambda s: s.encode('utf-16-le')
if u16('FileVersion') not in d:
    print('ERROR: VERSIONINFO resource missing'); sys.exit(1)
if u16(ver) not in d:
    print('ERROR: version string %r not found in VERSIONINFO' % ver); sys.exit(1)
print('versioninfo check:', ver, 'OK')
PYCHECK

# verify the exact export name XMPlay looks for (objdump lists "[index] name")
if ! $TRIPLE-objdump -p build/xmp-ffmpeg.dll | grep -qw "XMPIN_GetInterface"; then
	echo "ERROR: export 'XMPIN_GetInterface' missing:"; $TRIPLE-objdump -p build/xmp-ffmpeg.dll | sed -n '/Export Address Table/,/PE File Base Relocations/p'; exit 1
fi
echo "export check: XMPIN_GetInterface OK"

# bundle Mozilla's CA store next to the DLL (verified TLS); the plugin falls
# back to unverified connections when this file is missing
curl -sL --max-time 60 https://curl.se/ca/cacert.pem -o build/cacert.pem \
	&& echo "cacert.pem bundled ($(wc -c < build/cacert.pem) bytes)" \
	|| echo "WARNING: cacert.pem download failed - TLS will run unverified"

$TRIPLE-strip --strip-unneeded build/xmp-ffmpeg.dll
ls -la build/xmp-ffmpeg.dll
echo "done."
