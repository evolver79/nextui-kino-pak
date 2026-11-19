#!/bin/sh
# Kino: video player for NextUI. Plays what's under /mnt/SDCARD/Videos. Settings, resume positions and the
# thumbnail cache are kept in NextUI's user data folder.
cd "$(dirname "$0")"
mkdir -p "$USERDATA_PATH/Kino"
./kino "$USERDATA_PATH/Kino" "$PWD" > "$LOGS_PATH/Kino.txt" 2>&1
