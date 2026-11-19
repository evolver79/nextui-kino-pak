#!/bin/sh
# Builds Kino and copies it to the Brick with the brick tool (~/Code/brick-tool; IP and password in ~/.config/brick/config).
#   ./deploy.sh --bench   also copies test/*.mp4|mkv to /mnt/SDCARD/Videos/kino-test and plays each for 15 s (fps, CPU).
set -e
cd "$(dirname "$0")"
brick ping >/dev/null
build/build.sh
cp build/out/kino Kino.pak/kino
rm -rf Kino.pak/fonts && cp -r ui/dot/fonts Kino.pak/fonts
brick deploy Kino.pak >/dev/null
if [ "$1" = --bench ]; then
  brick push $(ls test/*.mp4 test/*.mkv) /mnt/SDCARD/Videos/kino-test >/dev/null
  brick ssh 'cd /mnt/SDCARD/Tools/tg5040/Kino.pak; for f in /mnt/SDCARD/Videos/kino-test/*; do ./kino --bench "$f" 15 2>/dev/null | grep ^bench; done'
fi
echo "Done. Open Tools > Kino on the Brick."
