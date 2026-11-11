#!/usr/bin/env bash
# build-ffmpeg.sh - build the cross toolchain and a lean, static, XP-targeting
# FFmpeg for the xmp-ffmpeg plugin.
#
# Toolchain + XP patches come from ffmpeg-windows-build-helpers (checkout it
# next to this repo or set BUILD_HELPERS_DIR) - the same proven recipe as our
# "FFmpeg for XP" builds: i686 MinGW-w64 9.0, GCC 10.4, win32 threads + winpthreads.
#
# FFmpeg is configured for DECODE-ONLY use: all demuxers/parsers/bitstream
# filters and all decoders incl. video (generated from the FFmpeg source) for
# the video-window variant, no encoders/filters/programs. swscale is enabled
# for video frame conversion/scaling. External libs: fdk-aac (better AAC),
# libcodec2, opencore-amr, gme (game music) + zlib/bzip2/lzma for containers.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT=$PWD
BH=${BUILD_HELPERS_DIR:-$ROOT/build-helpers}
PATCHES=$BH/patches
SANDBOX=$ROOT/sandbox
CROSS=$SANDBOX/cross_compilers/mingw-w64-i686
TRIPLE=i686-w64-mingw32
PREFIX=$CROSS/$TRIPLE
SRC=$SANDBOX/src
JOBS=${JOBS:-$(nproc)}
# Same target flags as our XP FFmpeg builds (SSE-only baseline, Athlon XP tuned).
# NOTE: no _WIN32_WINNT define - same as the main recipe. An explicit 0x0501
# hides Vista+ declarations that FFmpeg 9's own sources reference (e.g.
# WC_ERR_INVALID_CHARS in libavutil/wchar_filename.h) and breaks the build.
# XP compatibility is enforced by the patches + the PE import audit instead.
CFLAGS_XP="-O2 -march=pentium3 -mtune=athlon-xp -mfpmath=sse -msse"
DEFS_XP=""

export PATH=$CROSS/bin:$PATH
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig
export PKG_CONFIG_LIBDIR=

log() { printf '\n\033[1;33m== %s ==\033[0m\n' "$*"; }

fetch() { # fetch <url> <file>
	[[ -f $2 ]] && return 0
	log "downloading $(basename "$2")"
	local i
	for i in 1 2 3 4 5; do
		curl -sL --retry 3 --connect-timeout 20 -o "$2.part" "$1" && mv "$2.part" "$2" && return 0
		sleep 3
	done
	echo "FAILED to download $1" >&2
	exit 1
}

fetch_git() { # fetch_git <url> <dir> <commit>
	if [[ ! -d $2 ]]; then
		log "cloning $(basename "$1") @ ${3:0:7}"
		git clone -q "$1" "$2"
	fi
	git -C "$2" fetch -q origin "$3" 2>/dev/null || git -C "$2" fetch -q origin
	git -C "$2" checkout -q "$3"
}

# ---------------------------------------------------------------- 1. toolchain
if [[ ! -x $CROSS/bin/$TRIPLE-gcc ]]; then
	log "building MinGW-w64 cross toolchain (GCC 10.4, win32 threads) - one-time, slow"
	mkdir -p "$SANDBOX/cross_compilers"
	# config.guess fetch workaround (savannah is flaky), same trick as build.sh upstream
	fetch "http://git.savannah.gnu.org/gitweb/?p=config.git;a=blob_plain;f=config.guess;hb=HEAD" "$ROOT/config.guess" || true
	mkdir -p "$ROOT/bin"
	cat > "$ROOT/bin/wget" <<'WRAPPER'
#!/usr/bin/env bash
OUT=""
for arg in "$@"; do
	[[ $arg == http*config.guess* ]] && WANT=1
	[[ $arg == -O ]] && NEXT=1 && continue
	[[ -n ${NEXT:-} ]] && OUT=$arg && NEXT=
done
if [[ -n ${WANT:-} && -f /work-host/config.guess ]]; then cp /work-host/config.guess "$OUT"; exit 0; fi
exec /usr/bin/wget "$@"
WRAPPER
	chmod +x "$ROOT/bin/wget"
	cp "$PATCHES/mingw-w64-build-r33" "$SANDBOX/cross_compilers/"
	(
		cd "$SANDBOX/cross_compilers"
		unset CFLAGS CXXFLAGS LDFLAGS
		PATH="$ROOT/bin:$PATH" HOME=$SANDBOX ./mingw-w64-build-r33 \
			--build-type=win32 --default-configure --cpu-count=$JOBS \
			--pthreads-w32-ver=2-9-1 --disable-shared --clean-build --verbose
	)
	[[ -x $CROSS/bin/$TRIPLE-gcc ]] || { echo "toolchain build failed" >&2; exit 1; }
fi
$CROSS/bin/$TRIPLE-gcc --version | head -1

# ---------------------------------------------------------------------- 2. deps
mkdir -p "$SRC"
export CC=$TRIPLE-gcc AR=$TRIPLE-ar RANLIB=$TRIPLE-ranlib STRIP=$TRIPLE-strip
export CFLAGS="$CFLAGS_XP $DEFS_XP"
export CXXFLAGS="$CFLAGS"
unset LDFLAGS

# zlib
fetch "https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.xz" "$SRC/zlib-1.3.2.tar.xz"
if [[ ! -f $PREFIX/lib/libz.a ]]; then
	log "building zlib"
	tar -C "$SRC" -xf "$SRC/zlib-1.3.2.tar.xz"
	( cd "$SRC/zlib-1.3.2" && ./configure --static --prefix="$PREFIX" && make -j$JOBS && make install )
fi

# bzip2 (uses build-helpers' CMakeLists for clean cross builds)
fetch "https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz" "$SRC/bzip2-1.0.8.tar.gz"
if [[ ! -f $PREFIX/lib/libbz2.a ]]; then
	log "building bzip2"
	tar -C "$SRC" -xf "$SRC/bzip2-1.0.8.tar.gz"
	(
		cd "$SRC/bzip2-1.0.8"
		sed -i 's/WINAPI func/func/' bzlib.h
		cp -f "$PATCHES/bzip2_CMakeLists.txt" CMakeLists.txt
		cmake -B build -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$TRIPLE-gcc \
			-DCMAKE_RC_COMPILER=$TRIPLE-windres -DCMAKE_INSTALL_PREFIX="$PREFIX" .
		cmake --build build -j$JOBS && cmake --install build
	)
fi

# xz/lzma
fetch "https://sourceforge.net/projects/lzmautils/files/xz-5.8.3.tar.xz" "$SRC/xz-5.8.3.tar.xz"
if [[ ! -f $PREFIX/lib/liblzma.a ]]; then
	log "building xz (liblzma)"
	tar -C "$SRC" -xf "$SRC/xz-5.8.3.tar.xz"
	( cd "$SRC/xz-5.8.3" && ./configure --host=$TRIPLE --prefix="$PREFIX" --disable-shared \
		--disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo --disable-scripts --disable-doc --disable-nls \
		&& make -j$JOBS && make install )
fi

# fdk-aac (static) - same commit as our XP FFmpeg builds
fetch_git "https://github.com/mstorsjo/fdk-aac.git" "$SRC/fdk-aac_git" d8e6b1a3aa606c450241632b64b703f21ea31ce3
if [[ ! -f $PREFIX/lib/libfdk-aac.a ]]; then
	log "building fdk-aac"
	( cd "$SRC/fdk-aac_git" && ./autogen.sh >/dev/null 2>&1 || autoreconf -fi >/dev/null
	  ./configure --host=$TRIPLE --prefix="$PREFIX" --disable-shared --enable-static \
	  && make -j$JOBS && make install )
fi

# game-music-emu (static) - same commit as our XP FFmpeg builds
fetch_git "https://github.com/libgme/game-music-emu.git" "$SRC/game-music-emu_git" fe8da4b6d3876d7542c2fb69d94487e19836d678
if [[ ! -f $PREFIX/lib/libgme.a ]]; then
	log "building game-music-emu"
	(
		cd "$SRC/game-music-emu_git"
		sed -i '/EXCLUDE_FROM_ALL/d' CMakeLists.txt
		sed -i 's/gme ${libgme_SRCS}/gme STATIC ${libgme_SRCS}/' gme/CMakeLists.txt
		cmake -B build -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$TRIPLE-gcc \
			-DCMAKE_CXX_COMPILER=$TRIPLE-g++ -DCMAKE_RC_COMPILER=$TRIPLE-windres \
			-DBUILD_SHARED_LIBS=0 -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" .
		cmake --build build -j$JOBS && cmake --install build
	)
fi

# codec2 (static) - same commit as our XP FFmpeg builds
fetch_git "https://github.com/drowe67/codec2.git" "$SRC/codec2_git" 310777b1c6f1af0bc7c72f5b32f80f6fd9136962
if [[ ! -f $PREFIX/lib/libcodec2.a ]]; then
	log "building codec2"
	(
		cd "$SRC/codec2_git"
		sed -i '/install(SCRIPT ${CMAKE_BINARY_DIR}\/cmake\/GetDependencies.cmake)/d' CMakeLists.txt
		# codec2 builds a NATIVE codegen helper (generate_codebook). Two cross
		# contaminations must BOTH be cut here:
		#  1. the exported cross CC would turn the helper into generate_codebook.exe
		#     (the install copy looks for the extension-less name and fails),
		#  2. the exported cross CFLAGS (-march=pentium3...) reach the native
		#     compiler's probe compile and 64-bit gcc rejects them outright.
		# Force native compilers + empty flags via CMAKE_ARGS, and strip the
		# environment for the configure/build steps (explicit -D flags below keep
		# the cross flags for the real codec2 build).
		sed -i '/CMAKE_ARGS -DCMAKE_C_FLAGS=/d' src/CMakeLists.txt
		sed -i '/ExternalProject_Add(codec2_native/a\       CMAKE_ARGS -DCMAKE_C_FLAGS= -DCMAKE_CXX_FLAGS= -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++' src/CMakeLists.txt
		mkdir -p build && cd build
		env -u CFLAGS -u CXXFLAGS cmake .. -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$TRIPLE-gcc \
			-DCMAKE_CXX_COMPILER=$TRIPLE-g++ -DCMAKE_RC_COMPILER=$TRIPLE-windres \
			-DBUILD_SHARED_LIBS=0 -DUNITTEST=0 -DLPCNET=0 \
			-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS"
		env -u CFLAGS -u CXXFLAGS make -j$JOBS && make install
	)
fi

# opencore-amr (static) - same commit as our XP FFmpeg builds
fetch_git "https://github.com/BelledonneCommunications/opencore-amr.git" "$SRC/opencore-amr_git" 3b67218fb8efb776bcd79e7445774e02d778321d
if [[ ! -f $PREFIX/lib/libopencore-amrnb.a ]]; then
	log "building opencore-amr"
	( cd "$SRC/opencore-amr_git" && autoreconf -fi >/dev/null
	  ./configure --host=$TRIPLE --prefix="$PREFIX" --disable-shared --enable-static \
	  && make -j$JOBS && make install )
fi

# mbedtls 3.6.7 - TLS backend for https (HLS/DASH over https). Same pin and
# XP patches as our main XP FFmpeg builds (entropy via CryptGenRandom etc).
fetch_git "https://github.com/Mbed-TLS/mbedtls.git" "$SRC/mbedtls_git" 627361b09e6f6e3eac756297a370a591cf310ab9
if [[ ! -f $PREFIX/lib/libmbedtls.a ]]; then
	log "building mbedtls"
	(
		cd "$SRC/mbedtls_git"
		if [[ ! -d framework/.git ]]; then git submodule update --init framework; fi
		if [[ ! -f .xp-patched ]]; then
			patch -p1 < "$PATCHES/mbedtls-2.28.9_mingw-stdio.diff"
			patch -p1 < "$PATCHES/mbedtls-3.6.7_winxp-compatible-entropy.diff"
			touch .xp-patched
		fi
		mkdir -p build_dir && cd build_dir
		env -u CFLAGS -u CXXFLAGS cmake .. -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$TRIPLE-gcc \
			-DCMAKE_C_FLAGS="$CFLAGS -D__USE_MINGW_ANSI_STDIO=1" \
			-DENABLE_PROGRAMS=0 -DENABLE_TESTING=0 -DENABLE_ZLIB_SUPPORT=1 \
			-DCMAKE_INSTALL_PREFIX="$PREFIX"
		make -j$JOBS && make install
	)
fi

# ------------------------------------------------------------------- 3. FFmpeg
FFVER=9.0.2
FFSRC=$SRC/ffmpeg-$FFVER
fetch "https://ffmpeg.org/releases/ffmpeg-$FFVER.tar.xz" "$SRC/ffmpeg-$FFVER.tar.xz"
if [[ ! -d $FFSRC ]]; then
	log "unpacking FFmpeg $FFVER"
	tar -C "$SRC" -xf "$SRC/ffmpeg-$FFVER.tar.xz"
fi

# XP compatibility patches (same set as our XP FFmpeg builds; 0004/0005 are for
# shared-build DLL loading and not needed here)
if [[ ! -f $FFSRC/.xp-patched ]]; then
	log "applying XP patches"
	( cd "$FFSRC"
	  patch -p1 < "$PATCHES/0001-make-bcrypt-optional.patch"
	  patch -p1 < "$PATCHES/0002-windows-xp-compatible-CancelIoEx.patch"
	  patch -p1 < "$PATCHES/0003-windows-xp-compatible-wcscp.patch"
	  touch .xp-patched )
fi

# XP hardening beyond the main recipe: wchartocp() passes WC_ERR_INVALID_CHARS
# (Vista+) to WideCharToMultiByte; XP rejects unknown flags for CP_UTF8, so the
# wchar<->utf8 filename helpers would fail there. Force legacy conversion (0).
if grep -q 'WC_ERR_INVALID_CHARS' "$FFSRC/libavutil/wchar_filename.h"; then
	sed -i 's/WC_ERR_INVALID_CHARS/0x0/g' "$FFSRC/libavutil/wchar_filename.h"
	echo "patched wchar_filename.h: WC_ERR_INVALID_CHARS -> 0 (XP-safe)"
fi

if [[ ! -f $PREFIX/.ffmpeg-xmp-config-v4 ]]; then
	log "generating component lists (all decoders incl. video)"
	eval "$(python3 scripts/gen-component-lists.py "$FFSRC" | grep '^[A-Z]*=')"
	log "decoders: $(tr ',' '\n' <<<"$ALLDECODERS" | wc -l), demuxers: $(tr ',' '\n' <<<"$DEMUXERS" | wc -l)"

	log "configuring FFmpeg (static, decode-only, video enabled, XP target)"
	( cd "$FFSRC"
	  ./configure \
		--arch=x86 --target-os=mingw32 --cross-prefix=$CROSS/bin/$TRIPLE- \
		--prefix="$PREFIX" --pkg-config=pkg-config --pkg-config-flags=--static \
		--extra-cflags="$CFLAGS_XP $DEFS_XP" --extra-libs="-lssp -lpthread" \
		--disable-everything \
		--enable-decoder="$ALLDECODERS" \
		--enable-demuxer="$DEMUXERS" \
		--enable-parser="$PARSERS" \
		--enable-bsf="$BSFS" \
		--enable-protocol=file,http,https,tcp,tls,crypto,data,subfile \
		--enable-libfdk-aac --enable-libcodec2 --enable-libopencore-amrnb \
		--enable-libopencore-amrwb --enable-libgme \
		--enable-zlib --enable-bzlib --enable-lzma --enable-mbedtls \
		--enable-static --disable-shared \
		--disable-avdevice --disable-avfilter \
		--enable-swscale \
		--disable-programs --disable-doc --disable-debug \
		--disable-bcrypt --disable-w32threads \
		--disable-schannel \
		--enable-gpl --enable-version3 \
		--enable-nonfree \
		--extra-version=xmp )
	log "building FFmpeg"
	( cd "$FFSRC" && make -j$JOBS && make install )
	# config marker - bump the suffix whenever the configure options change so
	# stale cached FFmpeg builds get rebuilt with the new configuration.
	touch "$PREFIX/.ffmpeg-xmp-config-v4"
fi

log "done - FFmpeg libs in $PREFIX/lib"
