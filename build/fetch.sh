#!/bin/sh
# Downloads the library sources build/deps.sh builds (into deps/, once) and checks them.
set -e
cd "$(dirname "$0")/../deps"
get() {   # file url sha256
  [ -f "$1" ] || curl -fsSL -o "$1.part" "$2" && { [ -f "$1" ] || mv "$1.part" "$1"; }
  echo "$3  $1" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch: deps/$1" >&2; exit 1; }
}
get ffmpeg-5.1.4.tar.xz https://ffmpeg.org/releases/ffmpeg-5.1.4.tar.xz 54383bb890a1cd62580e9f1eaa8081203196ed53bde9e98fb6b0004423f49063
get freetype-2.13.2.tar.xz https://download.savannah.gnu.org/releases/freetype/freetype-2.13.2.tar.xz 12991c4e55c506dd7f9b765933e62fd2be2e06d421505d7950a132e4f1bb484d
get fribidi-1.0.13.tar.xz https://github.com/fribidi/fribidi/releases/download/v1.0.13/fribidi-1.0.13.tar.xz 7fa16c80c81bd622f7b198d31356da139cc318a63fc7761217af4130903f54a2
get harfbuzz-8.3.0.tar.xz https://github.com/harfbuzz/harfbuzz/releases/download/8.3.0/harfbuzz-8.3.0.tar.xz 109501eaeb8bde3eadb25fab4164e993fbace29c3d775bcaa1c1e58e2f15f847
get libass-0.15.2.tar.xz https://github.com/libass/libass/releases/download/0.15.2/libass-0.15.2.tar.xz 1be2df9c4485a57d78bb18c0a8ed157bc87a5a8dd48c661961c625cb112832fd
get mpv-0.35.1.tar.gz https://github.com/mpv-player/mpv/archive/refs/tags/v0.35.1.tar.gz 41df981b7b84e33a2ef4478aaf81d6f4f5c8b9cd2c0d337ac142fc20b387d1a9
