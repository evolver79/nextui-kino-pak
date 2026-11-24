// The Wi-Fi signal, read on its own thread (iw takes a few ms, too long to wait for on the drawing thread): at once when
// the kernel says the network changed (netlink: wlan0 up or down, an IP address got or lost), and every 3 s besides,
// since there is no event for the signal getting weaker. Connected means a link to the router and an IPv4 address:
// joined but still waiting on DHCP counts as off, as Jellyfin can't be reached yet.
// Weak below -72 dBm: from there a film streamed over Wi-Fi tends to stop to buffer.
#include "kino.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#define WEAK_DBM -72

static SDL_atomic_t state, dbm, drops;   // drops: times the link went from connected to gone
static int saved_ps = -1;   // power save before Kino turned it off: 1 on, 0 off, -1 not touched
// For the log (wifi_describe): the mesh node / router we're on, channel, link rate, and the radio's resends so far.
// The resends come from "iw station dump", asked only while a Jellyfin film plays (film: power save is off for it).
static SDL_mutex *desc_lock;
static SDL_atomic_t film;
static char d_ap[20], d_rate[16];
static int d_freq;
static long d_retries = -1, d_fails = -1;   // -1: not known (no film playing, or not asked yet)
static long last_retries = -1, last_fails = -1;   // at the last wifi_describe, or the film's first reading

static int has_ip(void) {   // wlan0 has an IPv4 address (DHCP done)
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return 1;   // can't tell: don't hold Jellyfin back over it
  struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
  snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "wlan0");
  ifr.ifr_addr.sa_family = AF_INET;
  int ok = ioctl(fd, SIOCGIFADDR, &ifr) == 0;
  close(fd);
  return ok;
}

static void read_once(void) {
  FILE *f = popen("/usr/sbin/iw dev wlan0 link 2>/dev/null", "r");
  if (!f) return;
  char line[160], ap[20] = "", rate[16] = ""; int connected = 0, sig = 0, freq = 0;
  while (fgets(line, sizeof line, f)) {
    if (!strncmp(line, "Connected to", 12)) { connected = 1; sscanf(line + 12, " %19s", ap); }
    char *p = strstr(line, "signal:");
    if (p) sig = atoi(p + 7);
    if ((p = strstr(line, "freq:"))) freq = atoi(p + 5);
    if ((p = strstr(line, "tx bitrate:"))) sscanf(p + 11, " %15s", rate);
  }
  pclose(f);
  long retries = -1, fails = -1;
  if (connected && SDL_AtomicGet(&film) && (f = popen("/usr/sbin/iw dev wlan0 station dump 2>/dev/null", "r"))) {
    while (fgets(line, sizeof line, f)) {
      char *p;
      if ((p = strstr(line, "tx retries:"))) retries = atol(p + 11);
      if ((p = strstr(line, "tx failed:"))) fails = atol(p + 10);
    }
    pclose(f);
  }
  if (desc_lock) {
    SDL_LockMutex(desc_lock);
    snprintf(d_ap, sizeof d_ap, "%s", ap); snprintf(d_rate, sizeof d_rate, "%s", rate);
    d_freq = freq; d_retries = retries; d_fails = fails;
    if (retries < 0) last_retries = last_fails = -1;   // no film: the next one counts from its own start
    else if (last_retries < 0) { last_retries = retries; last_fails = fails; }
    SDL_UnlockMutex(desc_lock);
  }
  static int linked;   // drops count the link to the router only, as before the address check
  if (linked && !connected) SDL_AtomicIncRef(&drops);
  linked = connected;
  int usable = connected && has_ip();
  SDL_AtomicSet(&dbm, usable ? sig : 0);
  SDL_AtomicSet(&state, !usable ? WIFI_OFF : sig && sig < WEAK_DBM ? WIFI_WEAK : WIFI_OK);
}

static int open_netlink(void) {   // link and IPv4 address changes; -1: no events, polling only
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (fd < 0) return -1;
  struct sockaddr_nl sa; memset(&sa, 0, sizeof sa);
  sa.nl_family = AF_NETLINK;
  sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;
  if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
  return fd;
}

static int run(void *ud) {
  (void)ud;
  int nl = open_netlink();
  for (;;) {
    read_once();
    if (nl < 0) { SDL_Delay(3000); continue; }
    struct pollfd p = { nl, POLLIN, 0 };
    if (poll(&p, 1, 3000) > 0) {
      SDL_Delay(200);   // changes come in bursts (link up, then the address): read once they've settled
      char buf[4096];
      while (recv(nl, buf, sizeof buf, MSG_DONTWAIT) > 0) {}   // what changed doesn't matter: iw is asked again
    }
  }
  return 0;
}

void wifi_start(void) {
  desc_lock = SDL_CreateMutex();
  SDL_Thread *t = SDL_CreateThread(run, "wifi", NULL);
  if (t) SDL_DetachThread(t);
}

int wifi_state(void) { return SDL_AtomicGet(&state); }
int wifi_dbm(void) { return SDL_AtomicGet(&dbm); }
int wifi_drops(void) { return SDL_AtomicGet(&drops); }

void wifi_power_save(int on) {
  SDL_AtomicSet(&film, !on);
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

// "node cd:e5:f6, 2452 MHz, -50 dBm, link 120 Mbit/s, 230 resends, 0 lost": the router's last three bytes (a mesh
// has one per node), and the radio's resends and lost packets since the last call (many: a busy or distant channel).
void wifi_describe(char *out, size_t n) {
  if (!desc_lock) { snprintf(out, n, "Wi-Fi not read"); return; }
  SDL_LockMutex(desc_lock);
  const char *ap = strlen(d_ap) > 8 ? d_ap + strlen(d_ap) - 8 : d_ap;
  int counted = last_retries >= 0 && d_retries >= last_retries && last_fails >= 0 && d_fails >= last_fails;
  if (!*d_ap) snprintf(out, n, "no Wi-Fi link");
  else if (!counted) snprintf(out, n, "node %s, %d MHz, %d dBm, link %s Mbit/s", ap, d_freq, wifi_dbm(), d_rate);   // the first of a film
  else snprintf(out, n, "node %s, %d MHz, %d dBm, link %s Mbit/s, %ld resends, %ld lost", ap, d_freq, wifi_dbm(), d_rate,
                d_retries - last_retries, d_fails - last_fails);
  if (d_retries >= 0) { last_retries = d_retries; last_fails = d_fails; }
  SDL_UnlockMutex(desc_lock);
}
