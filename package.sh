#!/bin/sh
# Builds dist/Kino.pak.zip with the pak's files at the top of the zip (the layout Pak Store expects).
# For a manual install, unzip it into a new folder: SD card Tools/tg5040/Kino.pak/.
set -e
cd "$(dirname "$0")"
build/build.sh >/dev/null
cp build/out/kino Kino.pak/kino
rm -rf Kino.pak/fonts && cp -r ui/dot/fonts Kino.pak/fonts
rm -rf Kino.pak/mpv && mkdir -p Kino.pak/mpv/fonts   # subtitles: mpv's config folder, Noto Sans in fonts/
cp deps/NotoSans-Regular.ttf deps/NotoSans-Bold.ttf Kino.pak/mpv/fonts/ && cp deps/NotoSans-OFL.txt Kino.pak/mpv/fonts/LICENSE-NotoSans.txt
rm -rf Kino.pak/licenses && cp -r licenses Kino.pak/licenses && cp LICENSE Kino.pak/LICENSE   # the program is GPL: licenses/README.txt
mkdir -p dist && rm -f dist/Kino.pak.zip
(cd Kino.pak && zip -q -X -r ../dist/Kino.pak.zip kino launch.sh fonts mpv licenses LICENSE)
unzip -l dist/Kino.pak.zip
