#!/bin/sh
# Builds Kino and copies it to the Brick over SSH (turn SSH on in the Brick first: Tools > SSH Server).
# Usage: ./deploy.sh [--bench] [ip] [password]     defaults: 192.168.68.196, tina
#   --bench also copies test/*.mp4|mkv to /mnt/SDCARD/Videos/kino-test and plays each for 20 s, printing fps and CPU.
set -e
cd "$(dirname "$0")"
BENCH=0; [ "$1" = --bench ] && { BENCH=1; shift; }
IP="${1:-192.168.68.196}"; PASS="${2:-tina}"
DIR="/mnt/SDCARD/Tools/tg5040/Kino.pak"
# ssh reads the password from an askpass helper, so no prompt or expect is needed.
ASK="$(mktemp)"; printf '#!/bin/sh\necho "%s"\n' "$PASS" > "$ASK"; chmod +x "$ASK"
trap 'rm -f "$ASK"' EXIT
export SSH_ASKPASS="$ASK" SSH_ASKPASS_REQUIRE=force DISPLAY="${DISPLAY:-x}"
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10"
build/build.sh
cp build/out/kino Kino.pak/kino
ssh $OPTS root@$IP "mkdir -p '$DIR' /mnt/SDCARD/Videos; rm -f '$DIR/kino'"
# the Brick has no scp: the files go over as a tar stream
COPYFILE_DISABLE=1 tar -C Kino.pak -cf - kino launch.sh | ssh $OPTS root@$IP "tar -xf - -C '$DIR'"
ssh $OPTS root@$IP "chmod +x '$DIR/kino' '$DIR/launch.sh'"
if [ $BENCH = 1 ]; then
  ssh $OPTS root@$IP "mkdir -p /mnt/SDCARD/Videos/kino-test"
  COPYFILE_DISABLE=1 tar -C test -cf - $(cd test && ls *.mp4 *.mkv) | ssh $OPTS root@$IP "tar -xf - -C /mnt/SDCARD/Videos/kino-test"
  # NextUI's own UI is still on screen; stop nothing, just play over it.
  ssh $OPTS root@$IP "cd '$DIR'; for f in /mnt/SDCARD/Videos/kino-test/*; do ./kino --bench \"\$f\" 20 2>/dev/null; done; cat /proc/cpuinfo | grep -m1 -i 'model name\|Hardware'; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null; ls /sys/class/led_anim/ 2>/dev/null"
fi
echo "Done. Open Tools > Kino on the Brick."
