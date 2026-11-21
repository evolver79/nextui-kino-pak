#!/bin/sh
# Builds build/out/kino for the TrimUI Brick (tg5040) in an ARM64 Debian bullseye container. The first run also builds
# FFmpeg, libass and libmpv as static libraries (build/deps.sh, several minutes); later runs reuse them. Only .a files
# are in build/out/prefix/lib, so those are linked statically; SDL2, zlib and libstdc++ come from the Brick.
set -e
cd "$(dirname "$0")/.."
sh build/fetch.sh
docker build -q -t nextui-kino-build build >/dev/null
docker run --rm -v "$PWD:/w" -w /w nextui-kino-build sh -c "
set -e
sh build/deps.sh >build/out/deps.log 2>&1 || { tail -40 build/out/deps.log; exit 1; }
export PKG_CONFIG_PATH=/w/build/out/prefix/lib/pkgconfig
gcc -O2 -Wall -o build/out/kino src/*.c src/vendor/cJSON.c ui/dot/src/dot.c -Iui/dot/src -Ibuild/out/prefix/include \$(pkg-config --cflags sdl2) \
  -Lbuild/out/prefix/lib -lmpv -lass -lharfbuzz -lfribidi -lfreetype -lavfilter -lpostproc -lavformat -lavcodec -lswresample -lswscale -lavutil \
  \$(pkg-config --libs sdl2) -lSDL2_ttf -lz -lstdc++ -latomic -lpthread -ldl -lrt -lm
cp build/out/kino build/out/kino.debug; strip build/out/kino
ls -la build/out/kino
echo needs: \$(objdump -p build/out/kino | grep NEEDED | tr -s ' ' | cut -d' ' -f3 | tr '\n' ' ')
"
