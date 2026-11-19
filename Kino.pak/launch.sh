#!/bin/sh
# Kino: video player for NextUI. Settings and resume positions are kept in NextUI's user data folder.
cd "$(dirname "$0")"
mkdir -p "$USERDATA_PATH/Kino"
./kino "$USERDATA_PATH/Kino" "$PWD" > "$LOGS_PATH/Kino.txt" 2>&1
