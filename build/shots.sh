#!/bin/sh
# Builds Kino, then renders its screens headless (SDL dummy video, software renderer, no sound) from the test videos
# in test/ into build/out/shots/*.png, to compare with the Dot style guide. The video tree is made of symlinks.
set -e
cd "$(dirname "$0")/.."
build/build.sh >/dev/null
O=build/out/shots
rm -rf $O && mkdir -p "$O/videos/Films" "$O/videos/Series/Northern" "$O/user"
# A Jellyfin login (copied from the Brick, never committed) adds shots of the real libraries and grid.
[ -f build/out/jfuser/jellyfin.txt ] && cp build/out/jfuser/jellyfin.txt "$O/user/"
ln -s ../../../../../test/hevc-1080p.mkv "$O/videos/Films/The Long Harbour.mkv"
ln -s ../../../../../test/h264-720p.mp4 "$O/videos/Films/Rally Highlights.mp4"
ln -s ../../../../../test/hevc-720p.mkv "$O/videos/Films/Glacier Walk.mkv"
ln -s ../../../../../test/h264-1080p.mp4 "$O/videos/Films/Night Ferry.mp4"
# A picture next to a film is its cover (here a frame saved as a jpg, to test the lookup).
ffmpeg -v error -ss 3 -i test/h264-480p.mp4 -frames:v 1 -vf scale=400:600 "$O/videos/Films/Rally Highlights.jpg"
# A wide film with its own dark grey bars (2.39:1 in a 16:9 file), for episode 3 below: the playing shots show the
# bars cut away (autocrop) and the subtitles moved up out of the bottom bar
[ -f build/out/letterbox.mkv ] || ffmpeg -v error -f lavfi -i testsrc2=s=1280x536:r=24:d=30 \
  -vf "pad=1280:720:0:92:color=0x101010" -c:v libx264 -pix_fmt yuv420p build/out/letterbox.mkv
i=1; for f in h264-480p.mp4 h264-720p.mp4 ../build/out/letterbox.mkv h264-1080p.mp4 hevc-1080p.mkv; do
  n=$(echo "Dark Snow Ice Thaw Light" | cut -d' ' -f$i)
  ln -s ../../../../../../test/$f "$O/videos/Series/Northern/Episode $i · $n.${f##*.}"; i=$((i+1))
done
# more episodes than fit on a screen: the list scrolls (the dotted track, "1–5 of 9")
for n in Frost Dusk Embers Dawn; do ln -s ../../../../../../test/h264-480p.mp4 "$O/videos/Series/Northern/Episode $i · $n.mp4"; i=$((i+1)); done
ln -s ../../../../test/h264-480p.mp4 "$O/videos/Northern Lights Timelapse.mp4"
# Subtitles next to the episode the playing shots use (episode 3, resumed at 42 %): shows the subtitle font works
printf '1\n00:00:00,000 --> 01:00:00,000\nThe ice had held all winter.\nNow it sang under our boots.\n' > "$O/videos/Series/Northern/Episode 3 · Ice.srt"
docker run --rm -v "$PWD:/w" -w /w -e SDL_VIDEODRIVER=dummy -e SDL_AUDIODRIVER=dummy nextui-kino-build sh -c "
mkdir -p $O/pak/mpv/fonts && cp -r ui/dot/fonts $O/pak/ && cp deps/NotoSans-Regular.ttf deps/NotoSans-Bold.ttf $O/pak/mpv/fonts/
build/out/kino $O/user $O/pak --shots $O $O/videos"
for f in $O/*.bmp; do ffmpeg -v error -y -i "$f" "${f%.bmp}.png" && rm "$f"; done
ls $O/*.png
