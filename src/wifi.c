// The Wi-Fi signal, read every 3 s on its own thread (iw takes a few ms, too long to wait for on the drawing thread).
// Weak below -72 dBm: from there a film streamed over Wi-Fi tends to stop to buffer.
#include "kino.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEAK_DBM -72

static SDL_atomic_t state, dbm, drops;   // drops: times the link went from connected to gone
static int saved_ps = -1;   // power save before Kino turned it off: 1 on, 0 off, -1 not touched

static void read_once(void) {
  FILE *f = popen("/usr/sbin/iw dev wlan0 link 2>/dev/null", "r");
  if (!f) return;
  char line[160]; int connected = 0, sig = 0;
  while (fgets(line, sizeof line, f)) {
    if (!strncmp(line, "Connected to", 12)) connected = 1;
    char *p = strstr(line, "signal:");
    if (p) sig = atoi(p + 7);
  }
  pclose(f);
  SDL_AtomicSet(&dbm, connected ? sig : 0);
  int was = SDL_AtomicGet(&state);
  if (!connected && (was == WIFI_OK || was == WIFI_WEAK)) SDL_AtomicIncRef(&drops);
  SDL_AtomicSet(&state, !connected ? WIFI_OFF : sig && sig < WEAK_DBM ? WIFI_WEAK : WIFI_OK);
}

static int run(void *ud) {
  (void)ud;
  for (;;) { read_once(); SDL_Delay(3000); }
  return 0;
}

void wifi_start(void) {
  SDL_Thread *t = SDL_CreateThread(run, "wifi", NULL);
  if (t) SDL_DetachThread(t);
}

int wifi_state(void) { return SDL_AtomicGet(&state); }
int wifi_dbm(void) { return SDL_AtomicGet(&dbm); }
int wifi_drops(void) { return SDL_AtomicGet(&drops); }

void wifi_power_save(int on) {
  if (!on) {
    if (saved_ps >= 0) return;
    FILE *f = popen("/usr/sbin/iw dev wlan0 get power_save 2>/dev/null", "r");
    char line[64] = "";
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; pclose(f); }
    saved_ps = strstr(line, ": on") ? 1 : 0;
    if (saved_ps) system("/usr/sbin/iw dev wlan0 set power_save off 2>/dev/null");
  } else if (saved_ps >= 0) {
    if (saved_ps) system("/usr/sbin/iw dev wlan0 set power_save on 2>/dev/null");
    saved_ps = -1;
  }
}
