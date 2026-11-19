#!/bin/sh
# Kino: video player for NextUI. Settings and resume positions are kept in NextUI's user data folder.
# For now (test player, no browser yet): plays the first video under /mnt/SDCARD/Videos.
cd "$(dirname "$0")"
mkdir -p "$USERDATA_PATH/Kino"
FILE="$(find /mnt/SDCARD/Videos -type f \( -iname '*.mp4' -o -iname '*.mkv' -o -iname '*.avi' -o -iname '*.m4v' -o -iname '*.webm' \) | sort | head -1)"
./kino "$FILE" > "$LOGS_PATH/Kino.txt" 2>&1
