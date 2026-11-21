#!/bin/sh
# Downloads the library sources build/deps.sh builds, and the subtitle font (into deps/, once), and checks them.
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
# Subtitles: Noto Sans (SIL OFL), made to read easily and with Latin, Greek and Cyrillic; not Dot's fonts on purpose
get NotoSans-Regular.ttf https://raw.githubusercontent.com/notofonts/notofonts.github.io/28b15b4b43b7bed62b5cf6e6b0b5ff5846270535/fonts/NotoSans/hinted/ttf/NotoSans-Regular.ttf 478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823
get NotoSans-Bold.ttf https://raw.githubusercontent.com/notofonts/notofonts.github.io/28b15b4b43b7bed62b5cf6e6b0b5ff5846270535/fonts/NotoSans/hinted/ttf/NotoSans-Bold.ttf 1df075a380fc7cb898acf64c1f7b3b4dd780de3caa860178bf929de35817a913
get NotoSans-OFL.txt https://raw.githubusercontent.com/notofonts/latin-greek-cyrillic/4bc63d7ebca1faed49c6c685f380ba0abc2c1941/OFL.txt cee9892f9f0cc8fe882c9e9537ee6a89621d86ee7ceaf70b02e2b2b1c25c061a
