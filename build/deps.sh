#!/bin/sh
# Runs inside the build container: builds FreeType, FriBidi, HarfBuzz, libass, FFmpeg and libmpv as static libraries into
# build/out/prefix. Each library is built once; delete build/out/prefix/.<name> to build it again.
set -e
P=/w/build/out/prefix
S=/w/build/out/src
mkdir -p $P $S
export PKG_CONFIG_PATH=$P/lib/pkgconfig PKG_CONFIG="pkg-config --static" CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC"
J=-j8

unpack() { rm -rf $S/$1 && tar xf /w/deps/$2 -C $S; }

if [ ! -f $P/.freetype ]; then
  unpack freetype-2.13.2 freetype-2.13.2.tar.xz && cd $S/freetype-2.13.2
  ./configure -q --prefix=$P --disable-shared --enable-static --with-zlib=no --with-bzip2=no --with-png=no \
    --with-harfbuzz=no --with-brotli=no >/dev/null
  make -s $J >/dev/null && make -s install >/dev/null && touch $P/.freetype
fi

if [ ! -f $P/.fribidi ]; then
  unpack fribidi-1.0.13 fribidi-1.0.13.tar.xz && cd $S/fribidi-1.0.13
  meson setup build --prefix=$P --libdir=lib --default-library=static -Ddocs=false -Dbin=false -Dtests=false >/dev/null
  ninja -C build >/dev/null && ninja -C build install >/dev/null && touch $P/.fribidi
fi

if [ ! -f $P/.harfbuzz ]; then
  unpack harfbuzz-8.3.0 harfbuzz-8.3.0.tar.xz && cd $S/harfbuzz-8.3.0
  meson setup build --prefix=$P --libdir=lib --default-library=static --auto-features=disabled -Dfreetype=enabled \
    -Dtests=disabled -Ddocs=disabled -Dutilities=disabled >/dev/null
  ninja -C build >/dev/null && ninja -C build install >/dev/null && touch $P/.harfbuzz
fi

if [ ! -f $P/.libass ]; then
  unpack libass-0.15.2 libass-0.15.2.tar.xz && cd $S/libass-0.15.2
  ./configure -q --prefix=$P --disable-shared --enable-static --disable-fontconfig \
    --disable-require-system-font-provider --disable-asm >/dev/null
  make -s $J >/dev/null && make -s install >/dev/null && touch $P/.libass
fi

if [ ! -f $P/.ffmpeg ]; then
  unpack ffmpeg-5.1.4 ffmpeg-5.1.4.tar.xz && cd $S/ffmpeg-5.1.4
  # Every decoder, demuxer, parser and filter; nothing that writes, no devices, no hardware decoding.
  ./configure --prefix=$P --enable-static --disable-shared --enable-gpl --enable-pic \
    --disable-programs --disable-doc --disable-debug --disable-encoders --disable-muxers --disable-devices \
    --disable-hwaccels --disable-vaapi --disable-vdpau --disable-xlib --disable-libxcb --disable-sdl2 \
    --disable-iconv --disable-lzma --disable-bzlib --disable-network --enable-zlib >/dev/null
  make -s $J >/dev/null && make -s install >/dev/null && touch $P/.ffmpeg
fi

if [ ! -f $P/.mpv ]; then
  unpack mpv-0.35.1 mpv-0.35.1.tar.gz && cd $S/mpv-0.35.1
  # libmpv only; video comes out through the software render API, sound through SDL2 (which the Brick has).
  meson setup build --prefix=$P --libdir=lib --default-library=static --auto-features=disabled \
    -Dlibmpv=true -Dcplayer=false -Dbuild-date=false -Dmanpage-build=disabled -Dhtml-build=disabled \
    -Dpdf-build=disabled -Dsdl2=enabled -Dsdl2-audio=enabled -Dzlib=enabled -Diconv=disabled -Dgl=disabled -Dvulkan=disabled -Dlua=disabled -Djavascript=disabled
  ninja -C build >/dev/null && ninja -C build install >/dev/null && touch $P/.mpv
fi
echo deps ok
