#!/bin/sh
# Kino: video player for NextUI. Plays what's under /mnt/SDCARD/Videos. Settings, resume positions and the
# thumbnail cache are kept in NextUI's user data folder.
cd "$(dirname "$0")"
mkdir -p "$USERDATA_PATH/Kino"
# The log of the run before is kept as Kino.prev.txt (a problem, then Kino started again to look: still there)
[ -f "$LOGS_PATH/Kino.txt" ] && mv -f "$LOGS_PATH/Kino.txt" "$LOGS_PATH/Kino.prev.txt"
./kino "$USERDATA_PATH/Kino" "$PWD" > "$LOGS_PATH/Kino.txt" 2>&1
