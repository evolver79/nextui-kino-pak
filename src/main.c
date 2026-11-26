// Kino: video player for NextUI on the TrimUI Brick, drawn with the shared Dot UI (ui/dot).
//   kino <user_dir> <pak_dir>                              the app: browse SD card/Videos, play, options, resume
//   kino <user_dir> <pak_dir> --shots <out_dir> <videos>   renders the screens to BMPs (headless, for review)
//   kino --bench <file> <seconds>                          plays a file, then prints frames, drops and CPU
#define _GNU_SOURCE
#include "kino.h"
#include "dot.h"
#include "jellyfin.h"
#include <ctype.h>
#include <dirent.h>
#include <execinfo.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>

enum { BTN_A = SDL_CONTROLLER_BUTTON_B, BTN_B = SDL_CONTROLLER_BUTTON_A, BTN_X = SDL_CONTROLLER_BUTTON_Y,
       BTN_Y = SDL_CONTROLLER_BUTTON_X, BTN_START = SDL_CONTROLLER_BUTTON_START, BTN_SELECT = SDL_CONTROLLER_BUTTON_BACK,
       BTN_MENU = SDL_CONTROLLER_BUTTON_GUIDE, BTN_L1 = SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BTN_R1 = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
       BTN_UP = SDL_CONTROLLER_BUTTON_DPAD_UP, BTN_DOWN = SDL_CONTROLLER_BUTTON_DPAD_DOWN,
       BTN_LEFT = SDL_CONTROLLER_BUTTON_DPAD_LEFT, BTN_RIGHT = SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
       BTN_L2 = SDL_CONTROLLER_BUTTON_MAX + 1, BTN_R2 = SDL_CONTROLLER_BUTTON_MAX + 2 };   // triggers are axes in SDL
enum { SCR_BROWSE, SCR_PLAY, SCR_OPTIONS, SCR_CONNECT, SCR_HELP, SCR_NEXT };
enum { O_ASPECT, O_BRIGHT, O_CONTRAST, O_DEINT, O_FILTER, O_ATRACK, O_APREFER, O_APREFER2, O_SUBS, O_TRACK, O_PREFER, O_PREFER2,
       O_HWDEC, O_QUALITY, O_AUTONEXT, NOPT };   // Ground light: settings.txt only
// Options in tabs (L1/R1): each tab's options in order, the first one where a tab opens.
#define NTABS 4
static const char *TAB_NAMES[NTABS] = { "Picture", "Audio", "Subtitles", "Brick" };
static const int TAB_FIRST[NTABS + 1] = { O_ASPECT, O_ATRACK, O_SUBS, O_HWDEC, NOPT };
static int opt_tab(int o) { int t = 0; while (t + 1 < NTABS && o >= TAB_FIRST[t + 1]) t++; return t; }
#define ROWS ((DOT_CONTENT_BOTTOM - 96 + 12) / (DOT_ROW_H + 12))   // list rows: Brick 5, Smart Pro 4
#define ROWS_MAX 5                    // for arrays: ROWS is at most this
#define CW DOT_CONTENT_W              // a full-width card: 944 / 1200
#define RIGHT (DOT_W - DOT_MARGIN)    // the content's right edge: 984 / 1240
#define MAXDEPTH 32

typedef struct {
  char name[256]; int dir, virt;   // virt: the Jellyfin row at the SD card root
  time_t mtime;                    // "Recently added" in a local grid
  char cover[1100]; int cover_set; // local grid: a picture next to it, "frame:<video>", or "" (worked out once)
} Item;

static SDL_Renderer *ren;
static Dot *dot;
static int running = 1, screen = SCR_BROWSE, opt_back, opt_sel, opt_resume_play, help_back;
static char root[1024], home[1024], cwd[1100];   // root: the SD card; home: its Videos folder, where Kino starts
static Item *items;
static int nitems, cursor, top, depth, stack[MAXDEPTH];
static char playing[1400], playing_title[256];
static Uint32 overlay_until, toast_until, seek_until;
static int seek_dir;   // -1/1: the rewind/forward button shows the last seek for a moment
static int on_bar;     // overlay focus on the progress bar (scrubbing); scrub_to = where A would jump
static double scrub_to;
// A notice in the middle (Dot's dot_notice) after a choice: filter, aspect, subtitles, sort. notice_at: when (0 none).
static char notice_label[32], notice_value[64];
static int notice_icon, notice_at_n, notice_n;
static Uint32 notice_at;
static int opening;        // a film asked for and not playing yet: the busy card says STARTING
static int sound_change;   // the sound track is being changed (asking the server for the stream again)
static int lost_tries; static Uint32 retry_at;   // a Jellyfin stream that broke off: opened again from where it was
// The network during this Jellyfin film: stalls (stops to buffer once playing), Wi-Fi drops since it started, and
// whether Kino has said the Wi-Fi can't keep up (then the buffering pill says so too, for the rest of the film).
static int net_stalls, net_drops0, net_poor, net_was_buffering;
#define LOST_TRIES 5
static Uint32 net_t0;   // when this Jellyfin film was asked for; net: log lines count from it
static const char *net_shots;   // KINO_SHOTS=<dir>: a screenshot at each warning (test/net.sh)
static int net_test;            // KINO_PLAY set: test/net.sh is driving (the position goes in the log every 10 s)
static volatile sig_atomic_t net_reset;   // SIGUSR1 (test/net.sh, next scenario): warn again as if a new film
static void on_usr1(int sig) { (void)sig; net_reset = 1; }
// Hang watchdog: the main loop not coming round for 15 s writes where it's stuck (backtrace) to the log, once.
static volatile Uint32 loop_tick;
static pthread_t main_thread;
static void on_usr2(int sig) {
  (void)sig;
  static const char m[] = "watchdog: main loop stuck 15 s, backtrace:\n";
  write(2, m, sizeof m - 1);
  void *bt[48]; int n = backtrace(bt, 48);
  backtrace_symbols_fd(bt, n, 2);
}
static int watchdog(void *u) {
  (void)u;
  int dumped = 0;
  for (;;) {
    SDL_Delay(1000);
    Uint32 t = loop_tick, age = SDL_GetTicks() - t;
    if (t && age > 15000 && !dumped) { dumped = 1; pthread_kill(main_thread, SIGUSR2); }
    else if (age < 2000 && dumped) { dumped = 0; fprintf(stderr, "watchdog: main loop going again\n"); }
  }
  return 0;
}
static volatile sig_atomic_t term_asked;   // SIGTERM (the system closing Kino): quit the normal way
static void on_term(int sig) { (void)sig; term_asked = 1; }
static void shot(const char *dir, const char *name);
// "net: ..." lines in the log (Kino.txt) while a Jellyfin film plays: what the network did and what Kino said.
static void net_log(const char *shot_name, const char *fmt, ...) {
  char m[200]; va_list a; va_start(a, fmt); vsnprintf(m, sizeof m, fmt, a); va_end(a);
  fprintf(stderr, "net: %6.1f %s\n", (SDL_GetTicks() - net_t0) / 1000.0, m);
  if (net_shots && shot_name) shot(net_shots, shot_name);
}
static SDL_Texture *crt_tex;
static char toast[160];
static SDL_Texture *thumb_tex;
static char thumb_for[1400];
static Uint32 thumb_accent;
static int folder_count = -1;
static Uint32 last_save;

// Jellyfin browsing: jf_mode lists jitems (mirrored into items for the shared list code); jstack = where we came from.
typedef struct { char id[40], type[32], name[160]; int cursor; } JfLevel;
static int jf_mode, jf_depth, jf_loading, jf_playing;
static JfLevel jstack[MAXDEPTH];
static JfItem *jitems, playing_jf;
static JfItem *jall;   // the whole level as listed; jitems is the view of it (the grid's sort / unwatched filter)
static int nall;
static Uint32 last_report;
// Background jobs (network): one at a time.
enum { J_NONE, J_CONNECT, J_POLL, J_LIST, J_PLAY, J_NEXT };
static SDL_Thread *job_th;
static SDL_atomic_t job_done;
static int job_kind, job_result, job_n;
static JfItem *job_items;
static char job_id[40], job_type[32], jf_code[16], jf_server[256], connect_msg[160];
static Uint32 next_poll;
static int jf_play_from_start, jf_transcoded;
static char jf_play_url[1400];
static int shots_mode;
// Poster grid (Jellyfin films and series): 2 rows of covers, 5 of 169 x 254 on the Brick, 7 of 153 x 230 on the Smart Pro
// (grid_fit, at the start), with room under them for the next row's top to peek out as a list's does (Dot's dot_grid:
// 40 px above the dock, fading out under it), and the row before peeking out under the header once scrolled; textures for the covers in view, by url.
static int GCOLS = 5, GPW = POSTER_W, GPH = POSTER_H;
#define GPAGE (GCOLS * 2)
#define GPAGE_MAX 16
static void grid_fit(void) {
  GPH = (DOT_DOCK_Y + 12 - 40 - 96 - 2 * 16) / 2;   // 40 of the peek above the dock, the rest fading under it if (GPH > POSTER_H) GPH = POSTER_H;
  GPW = GPH * POSTER_W / POSTER_H; GCOLS = (CW + 16) / (GPW + 16);
  if ((CW + 16) - GCOLS * (GPW + 16) > GPW / 2) {   // a lot left over: one more column, the covers a little smaller
    GCOLS++; GPW = (CW + 16) / GCOLS - 16; GPH = GPW * POSTER_H / POSTER_W;
  }
  if (GCOLS * 2 > GPAGE_MAX) GCOLS = GPAGE_MAX / 2;
}
static int jf_grid;
static int local_grid;   // an SD card folder of films (not episodes) shown as covers
typedef struct { char url[1100]; SDL_Texture *t; Uint32 used; } PosterTex;
static PosterTex ptex[32];
static Uint32 ptick;   // headless screens: no Jellyfin row (no network)

static const char *VIDEO_EXT[] = { "mp4", "mkv", "avi", "m4v", "webm", "mov", "ts", "m2ts", "mpg", "mpeg", "wmv", "flv",
                                   "3gp", "ogv", "vob", NULL };

// --- helpers --------------------------------------------------------------------------------------------------
static void fmt_time(double s, char *out, size_t n) {
  int t = s > 0 ? (int)s : 0;
  if (t >= 3600) snprintf(out, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
  else snprintf(out, n, "%02d:%02d", t / 60, t % 60);
}

static int natcmp(const char *a, const char *b) {
  while (*a && *b) {
    if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
      while (*a == '0' && isdigit((unsigned char)a[1])) a++;
      while (*b == '0' && isdigit((unsigned char)b[1])) b++;
      const char *sa = a, *sb = b;
      while (isdigit((unsigned char)*a)) a++;
      while (isdigit((unsigned char)*b)) b++;
      if (a - sa != b - sb) return a - sa < b - sb ? -1 : 1;
      int c = strncmp(sa, sb, a - sa); if (c) return c;
    } else {
      int c = tolower((unsigned char)*a) - tolower((unsigned char)*b); if (c) return c;
      a++; b++;
    }
  }
  return (unsigned char)*a - (unsigned char)*b;
}
static int item_cmp(const void *x, const void *y) {
  const Item *a = x, *b = y;
  return a->dir != b->dir ? b->dir - a->dir : natcmp(a->name, b->name);
}

static int mtime_cmp(const void *x, const void *y) {   // newest first
  time_t a = ((const Item *)x)->mtime, b = ((const Item *)y)->mtime;
  return a < b ? 1 : a > b ? -1 : 0;
}

static int is_video(const char *name) {
  const char *dot_ = strrchr(name, '.');
  if (!dot_) return 0;
  for (int i = 0; VIDEO_EXT[i]; i++) if (!strcasecmp(dot_ + 1, VIDEO_EXT[i])) return 1;
  return 0;
}

static void title_of(const char *name, char *out, size_t n) {
  snprintf(out, n, "%s", name);
  char *d = strrchr(out, '.'); if (d && d != out) *d = 0;
  for (char *p = out; *p; p++) if (*p == '_') *p = ' ';
}

static void item_path(int i, char *out, size_t n) { snprintf(out, n, "%s/%.255s", cwd, items[i].name); }

static int count_dir(const char *path) {
  DIR *d = opendir(path); if (!d) return 0;
  int n = 0; struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char p[1400]; struct stat st; snprintf(p, sizeof p, "%s/%s", path, e->d_name);
    if (stat(p, &st) == 0 && (S_ISDIR(st.st_mode) || is_video(e->d_name))) n++;
  }
  closedir(d);
  return n;
}

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

static int looks_episode(const char *n) {   // S01E02, 1x02, "Episode 3", "Ep 3"
  for (const char *p = n; *p; p++) {
    if ((*p == 's' || *p == 'S') && isdigit((unsigned char)p[1])) {
      const char *q = p + 1; while (isdigit((unsigned char)*q)) q++;
      if ((*q == 'e' || *q == 'E') && isdigit((unsigned char)q[1])) return 1;
    }
    if (isdigit((unsigned char)*p) && (p[1] == 'x' || p[1] == 'X') && isdigit((unsigned char)p[2]) && (p == n || !isalnum((unsigned char)p[-1]))) return 1;
    if (!strncasecmp(p, "episode", 7) || (!strncasecmp(p, "ep", 2) && (p[2] == ' ' || p[2] == '.') && isdigit((unsigned char)p[3]) && (p == n || !isalpha((unsigned char)p[-1])))) return 1;
  }
  return 0;
}

static const char *PICS[] = { "poster.jpg", "folder.jpg", "cover.jpg", "poster.png", "folder.png", "cover.png", NULL };
static int folder_picture(const char *dir, char *out, size_t n) {
  for (int i = 0; PICS[i]; i++) { snprintf(out, n, "%s/%s", dir, PICS[i]); if (file_exists(out)) return 1; }
  return 0;
}

static void item_path(int i, char *out, size_t n);
static const char *cover_of(int i) {   // a picture next to the film, else frames from it (folders: their first film)
  Item *it = &items[i];
  if (it->cover_set) return it->cover;
  it->cover_set = 1; it->cover[0] = 0;
  char p[1100], c[1100]; item_path(i, p, sizeof p);
  if (it->dir) {
    if (folder_picture(p, c, sizeof c)) { snprintf(it->cover, sizeof it->cover, "%s", c); return it->cover; }
    DIR *d = opendir(p); struct dirent *e; char first[256] = "";
    while (d && (e = readdir(d)))
      if (e->d_name[0] != '.' && is_video(e->d_name) && (!first[0] || natcmp(e->d_name, first) < 0)) snprintf(first, sizeof first, "%s", e->d_name);
    if (d) closedir(d);
    if (first[0]) snprintf(it->cover, sizeof it->cover, "frame:%.900s/%s", p, first);
    return it->cover;
  }
  static const char *SIDE[] = { ".jpg", ".png", "-poster.jpg", "-poster.png", NULL };
  char base[1100]; snprintf(base, sizeof base, "%s", p);
  char *dot_ = strrchr(base, '.'); if (dot_) *dot_ = 0;
  for (int k = 0; SIDE[k]; k++) {
    snprintf(c, sizeof c, "%.1000s%s", base, SIDE[k]);
    if (file_exists(c)) { snprintf(it->cover, sizeof it->cover, "%s", c); return it->cover; }
  }
  snprintf(it->cover, sizeof it->cover, "frame:%.1090s", p);
  return it->cover;
}

static int grid_mode(void) { return jf_mode ? jf_grid : local_grid; }
static int in_grid(void) { return grid_mode() && nitems && !(jf_mode && jf_loading); }

// --- browsing -------------------------------------------------------------------------------------------------
static void want_media(void) {
  if (!grid_mode()) media_want_posters(NULL, 0);
  if (!jf_mode && local_grid) {   // the covers in view and the next page; the focused film's facts for the header
    const char *urls[GPAGE_MAX * 2]; int n = 0;
    for (int i = top; i < nitems && i < top + GPAGE * 2; i++) if (*cover_of(i)) urls[n++] = cover_of(i);
    media_want_posters(urls, n);
    char sel[1400] = ""; const char *one[1] = { sel };
    folder_count = -1;
    if (cursor < nitems) {
      item_path(cursor, sel, sizeof sel);
      if (items[cursor].dir) folder_count = count_dir(sel);
    }
    media_want(NULL, one, cursor < nitems && !items[cursor].dir ? 1 : 0);
    return;
  }
  if (jf_mode && jf_grid) {   // the covers in view, then the next page
    static char buf[GPAGE_MAX * 2][600]; const char *urls[GPAGE_MAX * 2]; int n = 0;
    for (int i = top; i < nitems && i < top + GPAGE * 2; i++) {
      jf_poster_url(&jitems[i], POSTER_W, POSTER_H, buf[n], sizeof buf[n]); urls[n] = buf[n]; n++;
    }
    media_want(NULL, NULL, 0);
    media_want_posters(urls, n);
    return;
  }
  if (jf_mode) {   // the detail card's picture comes from the server (FFmpeg decodes the JPEG)
    char url[600] = "";
    if (cursor < nitems && strcmp(jitems[cursor].type, "Special")) jf_image_url(&jitems[cursor], THUMB_W, THUMB_H, url, sizeof url);
    media_want(url, NULL, 0);
    // the covers of the focused row and its neighbours: the blurred background behind the list
    static char buf[3][600]; const char *urls[3]; int n = 0;
    for (int i = cursor; i < nitems && i <= cursor + 1; i++) { jf_poster_url(&jitems[i], POSTER_W, POSTER_H, buf[n], sizeof buf[n]); urls[n] = buf[n]; n++; }
    if (cursor > 0) { jf_poster_url(&jitems[cursor - 1], POSTER_W, POSTER_H, buf[n], sizeof buf[n]); urls[n] = buf[n]; n++; }
    media_want_posters(urls, n);
    return;
  }
  const char *paths[ROWS_MAX]; static char buf[ROWS_MAX][1400];
  int n = 0;
  for (int i = top; i < nitems && i < top + ROWS; i++)
    if (!items[i].dir) { item_path(i, buf[n], sizeof buf[n]); paths[n] = buf[n]; n++; }
  char sel[1400] = "";
  if (cursor < nitems && !items[cursor].dir) item_path(cursor, sel, sizeof sel);
  media_want(sel, paths, n);
  folder_count = -1;
  if (cursor < nitems && items[cursor].dir && !items[cursor].virt) { item_path(cursor, sel, sizeof sel); folder_count = count_dir(sel); }
}

static void set_cursor(int c) {
  cursor = c < 0 ? 0 : c >= nitems ? (nitems ? nitems - 1 : 0) : c;
  if (grid_mode()) {   // the grid scrolls a row at a time
    int row = cursor / GCOLS, toprow = top / GCOLS;
    if (row < toprow) toprow = row;
    if (row > toprow + 1) toprow = row - 1;
    top = toprow * GCOLS;
  } else {
    if (cursor < top) top = cursor;
    if (cursor >= top + ROWS) top = cursor - ROWS + 1;
  }
  want_media();
}

static void list_dir(void) {
  free(items); items = NULL; nitems = 0;
  DIR *d = opendir(cwd);
  int cap = 0;
  struct dirent *e;
  while (d && (e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char p[1400]; struct stat st; snprintf(p, sizeof p, "%s/%s", cwd, e->d_name);
    if (stat(p, &st) != 0) continue;
    int dir = S_ISDIR(st.st_mode);
    if (!dir && !is_video(e->d_name)) continue;
    if (nitems == cap) { cap = cap ? cap * 2 : 64; items = realloc(items, cap * sizeof *items); }
    snprintf(items[nitems].name, sizeof items[nitems].name, "%s", e->d_name);
    items[nitems].mtime = st.st_mtime; items[nitems].cover_set = 0; items[nitems].virt = 0;
    items[nitems++].dir = dir;
  }
  if (d) closedir(d);
  if (nitems) qsort(items, nitems, sizeof *items, item_cmp);
  // Films (not episodes, not a tree of folders) as covers: most entries are videos or folders with a picture.
  local_grid = 0;
  if (depth > 0 && strcmp(cwd, home) && nitems >= 2) {
    int films = 0, eps = 0; char p[1400], c[1400];
    for (int i = 0; i < nitems; i++) {
      if (looks_episode(items[i].name)) eps++;
      snprintf(p, sizeof p, "%s/%s", cwd, items[i].name);
      if (!items[i].dir || folder_picture(p, c, sizeof c)) films++;
    }
    local_grid = !eps && films * 2 >= nitems;
  }
  if (local_grid && settings.sort == SORT_ADDED) qsort(items, nitems, sizeof *items, mtime_cmp);
  if (local_grid && settings.sort == SORT_UNWATCHED) {   // seen films leave the view (folders stay)
    int k = 0;
    for (int i = 0; i < nitems; i++) {
      char p[1400]; snprintf(p, sizeof p, "%s/%s", cwd, items[i].name);
      if (items[i].dir || store_resume(p, NULL, NULL) != RES_SEEN) items[k++] = items[i];
    }
    nitems = k;
  }
  if (!strcmp(cwd, home) && !shots_mode) {   // Videos, where Kino starts, begins with Jellyfin
    items = realloc(items, (nitems + 1) * sizeof *items);
    memmove(items + 1, items, nitems * sizeof *items);
    memset(items, 0, sizeof *items);
    snprintf(items[0].name, sizeof items[0].name, "Jellyfin");
    items[0].dir = items[0].virt = 1;
    nitems++;
  }
  top = 0;
  set_cursor(0);
}

// --- Jellyfin ---------------------------------------------------------------------------------------------------
static void show_toast(const char *s);
static void show_notice(int icon, const char *label, const char *value, int at, int n) {
  notice_icon = icon; notice_at_n = at; notice_n = n; notice_at = SDL_GetTicks();
  snprintf(notice_label, sizeof notice_label, "%s", label); snprintf(notice_value, sizeof notice_value, "%s", value);
}
// Up next: when an episode ends, the next one (the next file in an SD card series folder, or the server's next
// episode of the series) plays after a countdown; A plays it now, B stays in the browser. Options > Brick turns it off.
#define NEXT_MS 10000
static int next_kind;              // 0 none, 1 the SD card's next file (items[next_local]), 2 Jellyfin (next_jf)
static int next_local;
static JfItem next_jf;
static Uint32 next_at;             // when it starts by itself
static Uint32 next_leave; static int next_go;   // A/B or the countdown: the card going out (Dot's dot_rise), then go (or not)
static char next_pic[1100];        // its picture: the file's path, or the server's image URL

static int job_run(void *u) {
  (void)u;
  switch (job_kind) {
    case J_CONNECT:
      if (!jf_discover(jf_server, sizeof jf_server)) job_result = -2;
      else job_result = jf_qc_start(jf_server, jf_code, sizeof jf_code) ? 1 : -1;
      break;
    case J_POLL: job_result = jf_qc_poll(); break;
    case J_LIST: job_n = job_id[0] ? jf_children(job_id, job_type, &job_items) : jf_views(&job_items); break;
    case J_PLAY: job_result = jf_playback(&playing_jf, jf_play_url, sizeof jf_play_url, QUALITY_BPS[settings.quality]);
      if (job_result) jf_segments_load(playing_jf.id);   // intro, recap, credits: the skip button
      break;
    case J_NEXT: job_result = jf_next_episode(&playing_jf, &next_jf); break;
  }
  SDL_AtomicSet(&job_done, 1);
  return 0;
}
static void start_job(int kind) {
  if (job_th) return;
  job_kind = kind; SDL_AtomicSet(&job_done, 0);
  job_th = SDL_CreateThread(job_run, "job", NULL);
}

static int added_cmp(const void *x, const void *y) {   // newest first
  return strcmp(((const JfItem *)y)->added, ((const JfItem *)x)->added);
}
static int unseen(const JfItem *it) { return it->folder ? it->unplayed > 0 : !it->played; }

static void jf_view(const char *keep_id) {   // jall through the grid's sort into jitems / items
  free(jitems); jitems = calloc(nall > 0 ? nall : 1, sizeof *jitems);
  int n = 0;
  for (int i = 0; i < nall; i++)
    if (!jf_grid || settings.sort != SORT_UNWATCHED || unseen(&jall[i])) jitems[n++] = jall[i];
  if (jf_grid && settings.sort == SORT_ADDED) qsort(jitems, n, sizeof *jitems, added_cmp);   // Name: the server's order
  free(items); items = calloc(n > 0 ? n : 1, sizeof *items); nitems = n;
  int keep = 0;
  for (int i = 0; i < nitems; i++) {
    JfItem *it = &jitems[i];
    int special = jf_depth > 0 && !strcmp(jstack[jf_depth].type, "Special");
    if (!strcmp(it->type, "Episode") && special && it->series[0])   // Continue watching / Next up: the series first
      snprintf(items[i].name, sizeof items[i].name, "%s · S%d E%d", it->series, it->season, it->index);
    else if (!strcmp(it->type, "Episode") && it->index)
      snprintf(items[i].name, sizeof items[i].name, "%d · %s", it->index, it->name);
    else snprintf(items[i].name, sizeof items[i].name, "%s", it->name);
    items[i].dir = it->folder;
    if (keep_id && !strcmp(it->id, keep_id)) keep = i;
  }
  top = 0;
  set_cursor(keep);
}

static void jf_show(JfItem *list, int n) {   // the listed items become the browser's rows
  free(jall); jall = list; nall = n > 0 ? n : 0;
  // Films, series and collections as a poster grid; everything else (seasons, episodes, home videos) as rows.
  jf_grid = nall > 0;
  for (int i = 0; i < nall; i++)
    if (strcmp(jall[i].type, "Movie") && strcmp(jall[i].type, "Series") && strcmp(jall[i].type, "BoxSet")) jf_grid = 0;
  jf_view(NULL);
}

static void jf_sort_next(void) {   // SELECT in the grid: Name, Recently added, Unwatched
  char id[40] = ""; if (cursor < nitems) snprintf(id, sizeof id, "%s", jitems[cursor].id);
  settings.sort = (settings.sort + 1) % NSORT;
  settings_save();
  jf_view(id);
  show_notice(DOT_I_FILM, "SORT", SORT_NAMES[settings.sort], settings.sort + 1, NSORT);
}

static void jf_open(const char *id, const char *type, const char *name) {   // list a level (id "" = libraries)
  if (jf_depth + 1 >= MAXDEPTH) return;
  jf_grid = 0;
  JfLevel *l = &jstack[jf_depth];
  snprintf(l->id, sizeof l->id, "%s", id); snprintf(l->type, sizeof l->type, "%s", type);
  snprintf(l->name, sizeof l->name, "%s", name);
  snprintf(job_id, sizeof job_id, "%s", id); snprintf(job_type, sizeof job_type, "%s", type);
  jf_loading = 1;
  start_job(J_LIST);
}

// No Wi-Fi link (off in the device settings, or no network in range): the Jellyfin row is greyed out and can't be
// opened. Unknown (the first reading not in yet, or no Wi-Fi thread on headless screens) counts as there.
static int jf_offline(void) { return wifi_state() == WIFI_OFF; }

static void jf_enter(void) {   // from the SD card root
  if (jf_offline()) { show_toast("No Wi-Fi: turn it on to use Jellyfin"); return; }
  if (!jf_logged_in()) {
    screen = SCR_CONNECT; jf_code[0] = 0;
    snprintf(connect_msg, sizeof connect_msg, "Looking for Jellyfin on the network…");
    start_job(J_CONNECT);
    return;
  }
  jf_mode = 1; jf_depth = 0; jstack[0].cursor = 0;
  free(items); items = NULL; nitems = 0;
  jf_open("", "", "Jellyfin");
  if (wifi_state() == WIFI_WEAK) show_toast("Weak Wi-Fi: films may stop to buffer");
}

static void jf_leave_level(void) {
  if (jf_depth == 0) {   // back to the SD card
    jf_mode = 0; jf_grid = 0; free(jitems); jitems = NULL; free(jall); jall = NULL; nall = 0;
    list_dir();
    return;
  }
  jf_depth--;
  int c = jstack[jf_depth].cursor;
  JfLevel *l = &jstack[jf_depth];
  char id[40], type[24], name[160];
  snprintf(id, sizeof id, "%s", l->id); snprintf(type, sizeof type, "%s", l->type); snprintf(name, sizeof name, "%s", l->name);
  jf_open(id, type, name);
  jstack[jf_depth].cursor = c;
}

static void stream_lost(const char *how);
static void job_finished(void) {
  SDL_WaitThread(job_th, NULL); job_th = NULL;
  if (job_kind == J_CONNECT) {
    if (job_result == 1) { snprintf(connect_msg, sizeof connect_msg, "%s", jf_server); next_poll = SDL_GetTicks() + 2000; }
    else snprintf(connect_msg, sizeof connect_msg, job_result == -2 ? "No Jellyfin server answered on this network."
                                                                    : "The server didn't start Quick Connect. Is it turned on?");
  } else if (job_kind == J_POLL) {
    if (job_result == 1) { if (screen == SCR_CONNECT) { screen = SCR_BROWSE; jf_enter(); } }
    else if (job_result < 0) { jf_code[0] = 0; snprintf(connect_msg, sizeof connect_msg, "The code ran out. Press A for a new one."); }
    else next_poll = SDL_GetTicks() + 2000;
  } else if (job_kind == J_PLAY) {   // the server said how: direct play or its transcode
    jf_transcoded = job_result == 2;
    snprintf(playing, sizeof playing, "%s", jf_play_url);
    const char *su, *st, *sl;
    for (int i = 0; jf_subtitle(i, &su, &st, &sl); i++) player_add_sub(su, st, sl);
    double start = jf_play_from_start ? 0 : playing_jf.pos;
    if (lost_tries && !job_result) {   // reconnecting and the server didn't answer: no point opening the film and waiting
      player_close(); stream_lost("server didn't answer"); return;
    }
    { char w[120]; wifi_describe(w, sizeof w); net_log(NULL, "Wi-Fi: %s", w); }
    net_log(NULL, "open at %.0f s, %s", start, job_result == 2 ? "transcoded" : job_result == 1 ? "direct" : "server didn't answer");
    player_open(playing, start);
    wifi_power_save(0);
    if (!lost_tries) jf_report(0, playing_jf.id, start, 0);
    if (screen == SCR_OPTIONS) player_pause(1);   // another sound track, chosen in Options: back to it paused
    else screen = SCR_PLAY;
    on_bar = 0;
    overlay_until = SDL_GetTicks() + DOT_AUTOHIDE_MS;
    last_save = last_report = SDL_GetTicks();
    sound_change = 0;
    if (!lost_tries && wifi_state() == WIFI_WEAK) { show_toast("Weak Wi-Fi: this film may stop to buffer"); net_log("weak", "toast: weak Wi-Fi"); }
  } else if (job_kind == J_NEXT) {
    if (job_result == 1 && screen == SCR_BROWSE) {
      next_kind = 2; next_at = SDL_GetTicks() + NEXT_MS; screen = SCR_NEXT;
      jf_image_url(&next_jf, THUMB_W, THUMB_H, next_pic, sizeof next_pic);
      media_want(next_pic, NULL, 0);
    }
  } else if (job_kind == J_LIST) {
    jf_loading = 0;
    if (job_n < 0) {
      show_toast(jf_logged_in() ? "Can't reach Jellyfin" : "Logged out of Jellyfin");
      if (!jf_logged_in()) { jf_depth = 0; jf_leave_level(); }
      return;
    }
    // A series with one season opens straight on its episodes.
    if (!strcmp(job_type, "Series") && job_n == 1 && !strcmp(job_items[0].type, "Season")) {
      snprintf(job_id, sizeof job_id, "%s", job_items[0].id); snprintf(job_type, sizeof job_type, "Season");
      free(job_items); job_items = NULL;
      jf_loading = 1; start_job(J_LIST);
      return;
    }
    int keep = jstack[jf_depth].cursor;
    jf_show(job_items, job_n); job_items = NULL;
    set_cursor(keep);
  }
}



static void local_sort_next(void) {   // SELECT in a local grid: as in Jellyfin
  char keep[256] = ""; if (cursor < nitems) snprintf(keep, sizeof keep, "%s", items[cursor].name);
  settings.sort = (settings.sort + 1) % NSORT;
  settings_save();
  list_dir();
  for (int i = 0; i < nitems; i++) if (!strcmp(items[i].name, keep)) { set_cursor(i); break; }
  show_notice(DOT_I_FILM, "SORT", SORT_NAMES[settings.sort], settings.sort + 1, NSORT);
}

static void enter_dir(void) {
  if (items[cursor].virt) { jf_enter(); return; }
  if (depth + 1 >= MAXDEPTH) return;
  stack[depth++] = cursor;
  size_t l = strlen(cwd);
  snprintf(cwd + l, sizeof cwd - l, "/%s", items[cursor].name);
  list_dir();
}

static void leave_dir(void) {
  if (!depth) return;
  char *s = strrchr(cwd, '/'), left[256] = "";
  if (s) { snprintf(left, sizeof left, "%s", s + 1); *s = 0; }
  list_dir();
  int c = stack[--depth];   // back on the folder we came out of (found by name: from Videos up, there's no stack entry)
  for (int i = 0; i < nitems; i++) if (!items[i].virt && !strcmp(items[i].name, left)) { c = i; break; }
  set_cursor(c);
}

static void select_path(const char *path) {   // put the cursor on this file if it's in the list
  for (int i = 0; i < nitems; i++) {
    char p[1400]; item_path(i, p, sizeof p);
    if (!strcmp(p, path)) { set_cursor(i); return; }
  }
}

// --- playing --------------------------------------------------------------------------------------------------
static void show_toast(const char *s) { snprintf(toast, sizeof toast, "%s", s); toast_until = SDL_GetTicks() + 2600; }

static void play_jf(const JfItem *it, int from_start) {   // first ask the server how, on the worker
  if (job_th) return;
  playing_jf = *it; jf_playing = 1; jf_play_from_start = from_start;
  jf_audio_want = -1;   // a new film: its sound by Prefer (or the server's pick), not the last film's choice
  snprintf(jf_audio_prefer, sizeof jf_audio_prefer, "%s%s%s", settings.aprefer1, *settings.aprefer2 ? "," : "", settings.aprefer2);
  net_stalls = net_poor = net_was_buffering = 0; net_drops0 = wifi_drops(); net_t0 = SDL_GetTicks();
  net_log(NULL, "play %s from %.0f s, quality %d", playing_jf.id, from_start ? 0 : playing_jf.pos, settings.quality);
  if (!strcmp(playing_jf.type, "Episode") && playing_jf.index)
    snprintf(playing_title, sizeof playing_title, "S%d · E%d · %s", playing_jf.season, playing_jf.index, playing_jf.name);
  else snprintf(playing_title, sizeof playing_title, "%s", playing_jf.name);
  toast_until = 0; opening = 1;   // the busy card says it's starting (busy_now)
  start_job(J_PLAY);
}

static void play(int i, int from_start) {
  media_want(NULL, NULL, 0);    // nothing in the background while a film plays: covers and pictures
  media_want_posters(NULL, 0);  // compete for the Wi-Fi and the CPU (they come back in the browser)
  if (jf_mode) {   // first ask the server how (direct or transcoded), on the worker; job_finished starts it
    play_jf(&jitems[i], from_start);
    return;
  }
  jf_playing = 0;
  item_path(i, playing, sizeof playing);
  title_of(items[i].name, playing_title, sizeof playing_title);
  double pos = 0;
  if (!from_start && store_resume(playing, &pos, NULL) != RES_PARTIAL) pos = 0;
  player_open(playing, pos);
  opening = 1;
  screen = SCR_PLAY;
  on_bar = 0;
  overlay_until = SDL_GetTicks() + DOT_AUTOHIDE_MS;
  last_save = SDL_GetTicks();
}

static void save_position(void) {
  double pos = player_pos(), dur = player_duration();
  if (jf_playing) return;   // Jellyfin keeps its own (reported while playing)
  if (dur > 0) { store_set_resume(playing, pos, dur); store_save(); }
}

static void stop(int ended) {
  on_bar = 0; lost_tries = 0; retry_at = 0;
  wifi_power_save(1);
  if (jf_playing) {
    double pos = ended ? playing_jf.runtime : player_pos();
    jf_report(2, playing_jf.id, pos, 0);
    if (!ended) player_close();
    for (int i = 0; i < nall && jf_mode; i++)   // the whole level too (the grid's view is made from it)
      if (!strcmp(jall[i].id, playing_jf.id)) {
        double len = jall[i].runtime;
        if (ended || (len > 0 && pos > len * .9)) { jall[i].played = 1; jall[i].pos = 0; } else jall[i].pos = pos;
      }
    for (int i = 0; i < nitems && jf_mode; i++)   // the list shows the new state before the next refresh
      if (!strcmp(jitems[i].id, playing_jf.id)) {
        double len = jitems[i].runtime;
        if (ended || (len > 0 && pos > len * .9)) { jitems[i].played = 1; jitems[i].pos = 0; }
        else jitems[i].pos = pos;
        set_cursor(ended && i + 1 < nitems ? i + 1 : i);
        if (!ended && jitems[i].pos > 0) show_toast("Saved where you stopped");
      }
    jf_playing = 0;
    screen = SCR_BROWSE;
    want_media();
    if (ended && settings.autonext && !strcmp(playing_jf.type, "Episode") && *playing_jf.series_id) start_job(J_NEXT);
    return;
  }
  if (ended) { store_set_resume(playing, -1, 0); store_save(); }
  else {
    save_position();
    if (store_resume(playing, NULL, NULL) == RES_PARTIAL) show_toast("Saved where you stopped");
    player_close();
  }
  screen = SCR_BROWSE;
  select_path(playing);
  int was = cursor;
  if (ended && cursor + 1 < nitems && !items[cursor + 1].dir) set_cursor(cursor + 1);   // A plays the next one
  want_media();
  const char *base = strrchr(playing, '/'); base = base ? base + 1 : playing;
  if (ended && settings.autonext && cursor == was + 1 && looks_episode(base) && looks_episode(items[cursor].name)) {
    next_kind = 1; next_local = cursor; next_at = SDL_GetTicks() + NEXT_MS; screen = SCR_NEXT;
    item_path(cursor, next_pic, sizeof next_pic);
  }
}

static void start_next(void) {   // Up next: play it (A, or the countdown ran out)
  int kind = next_kind; next_kind = 0;
  if (kind == 1 && next_local < nitems) { screen = SCR_BROWSE; play(next_local, 0); }
  else if (kind == 2) { screen = SCR_BROWSE; media_want(NULL, NULL, 0); media_want_posters(NULL, 0); play_jf(&next_jf, 0); }
  else screen = SCR_BROWSE;
}

// --- drawing --------------------------------------------------------------------------------------------------
// The picture on the info card, inset like everything else on a card (30 px); drawn a little smaller than it is made.
// On a shorter screen it keeps the card's width and is that much less high (the Smart Pro: 360 x 154, the middle of the
// picture), and what is under it moves up as much.
#define CARD_PIC_W 360
#define CARD_PIC_H (CARD_PIC_W * THUMB_H / THUMB_W - DY)
static void draw_thumb_area(int x, int y, const char *path, int is_dir) {
  int w = THUMB_W, h = THUMB_H;
  const uint8_t *rgb; Uint32 acc;
  if (!is_dir && strcmp(thumb_for, path)) {
    if (media_thumb(path, &rgb, &acc)) {
      if (!thumb_tex) thumb_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, w, h);
      SDL_UpdateTexture(thumb_tex, NULL, rgb, w * 3);
      snprintf(thumb_for, sizeof thumb_for, "%s", path);
      thumb_accent = acc;
    }
  }
  if (!is_dir && !strcmp(thumb_for, path) && thumb_tex) {
    SDL_Rect d = { x, y, CARD_PIC_W, CARD_PIC_H };
    int sh = THUMB_H * CARD_PIC_H / (CARD_PIC_W * THUMB_H / THUMB_W);   // a shorter card: the middle of the picture
    SDL_Rect src = { 0, (THUMB_H - sh) / 2, THUMB_W, sh };
    SDL_SetTextureBlendMode(thumb_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(thumb_tex, (Uint8)(dot_alpha(dot) * 255));   // fades with its card (Up next coming and going)
    SDL_RenderCopy(ren, thumb_tex, &src, &d);
    dot_corners(dot, x, y, CARD_PIC_W, CARD_PIC_H, 20, DOT_BG_GLASS);
  } else {
    // no picture: paper with the icon in ink at 80%, so it stands out from the card
    float a = dot_alpha(dot);
    dot_rect(dot, x, y, CARD_PIC_W, CARD_PIC_H, 20, DOT_PAPER, 255);
    dot_set_alpha(dot, a * .8f);
    dot_icon(dot, is_dir ? DOT_I_FOLDER : DOT_I_FILM, x + CARD_PIC_W / 2, y + CARD_PIC_H / 2, 72, DOT_INK);
    dot_set_alpha(dot, a);
  }
}

static void jf_label(const JfItem *it, char *out, size_t n) {   // "S1 · E3 · 44 MIN", "2021 · 1H 52M", "SERIES"
  char len[24] = "";
  int m = (int)(it->runtime / 60 + .5);
  if (m >= 60) snprintf(len, sizeof len, "%dh %02dm", m / 60, m % 60); else if (m) snprintf(len, sizeof len, "%d min", m);
  if (!strcmp(it->type, "Episode")) snprintf(out, n, "S%d · E%d%s%s", it->season, it->index, *len ? " · " : "", len);
  else if (!strcmp(it->type, "Movie") || !strcmp(it->type, "Video"))
    { if (it->year) snprintf(out, n, "%d%s%s", it->year, *len ? " · " : "", len); else snprintf(out, n, "%s", len); }
  else if (!strcmp(it->type, "Series")) snprintf(out, n, it->unplayed ? "Series · %d new" : "Series", it->unplayed);
  else if (!strcmp(it->type, "Special")) snprintf(out, n, "%s", !strcmp(it->id, "resume") ? "Films and episodes you started" : "The next episode of each series");
  else if (!strcmp(it->type, "Season")) snprintf(out, n, it->unplayed ? "Season · %d new" : "Season", it->unplayed);
  else if (!strncmp(it->type, "CollectionFolder", 16) || !strcmp(it->type, "UserView")) snprintf(out, n, "Library");
  else snprintf(out, n, "%s", it->folder ? "Folder" : len);
}

static void draw_detail_jf(void) {
  dot_card(dot, 40, 96, 420, 528 - DY);
  if (jf_loading || !nitems) {
    int all_seen = !jf_loading && jf_grid && settings.sort == SORT_UNWATCHED;
    draw_thumb_area(70, 126, "", 1);
    dot_text(dot, DOT_TITLE, jf_loading ? "Loading…" : all_seen ? "All seen" : "Nothing here", 70, 366 - DY, DOT_TEXT, DOT_LEFT, 360);
    if (all_seen) dot_text(dot, DOT_BODY_S, "Press SELECT to show everything.", 70, 420 - DY, DOT_LABEL, DOT_LEFT, 360);
    return;
  }
  JfItem *it = &jitems[cursor];
  char url[600], buf[256];
  if (strcmp(it->type, "Special")) jf_image_url(it, THUMB_W, THUMB_H, url, sizeof url); else url[0] = 0;
  draw_thumb_area(70, 126, url, url[0] == 0);
  int ep_of_series = !strcmp(it->type, "Episode") && it->series[0];   // episodes: the series is the title
  dot_text_scroll(dot, DOT_TITLE, ep_of_series ? it->series : it->name, 70, 366 - DY, DOT_TEXT, 360);   // too long: it scrolls
  jf_label(it, buf, sizeof buf);
  if (ep_of_series) { size_t l = strlen(buf); snprintf(buf + l, sizeof buf - l, " · %s", it->name); }
  dot_text(dot, DOT_LABEL_T, buf, 70, 420 - DY, DOT_LABEL, DOT_LEFT, 360);
  if (it->folder) return;
  Uint32 acc = !strcmp(thumb_for, url) ? thumb_accent : DOT_ACCENT_KINO;
  if (it->pos > 0 && !it->played) {
    dot_adot(dot, 70, 478 - DY, acc);
    dot_text(dot, DOT_LABEL_T, "Resume", 102, 478 - DY, DOT_TEXT, DOT_LEFT, 0);
    fmt_time(it->pos, buf, sizeof buf);
  } else {
    dot_text(dot, DOT_LABEL_T, it->played ? "Seen" : "Length", 70, 478 - DY, DOT_LABEL, DOT_LEFT, 0);
    if (it->runtime > 0) fmt_time(it->runtime, buf, sizeof buf); else snprintf(buf, sizeof buf, "--:--");
  }
  dot_time(dot, DOT_DISPLAY_L, buf, 68, 506 - DY, DOT_TEXT, DOT_LEFT);
  dot_progress(dot, 70, 578 - DY, 360, it->played ? 1 : it->runtime > 0 ? (float)(it->pos / it->runtime) : 0, 0);
}

static void draw_detail(void) {
  if (jf_mode) { draw_detail_jf(); return; }
  dot_card(dot, 40, 96, 420, 528 - DY);
  if (!nitems) {
    int all_seen = local_grid && settings.sort == SORT_UNWATCHED;
    draw_thumb_area(70, 126, "", 1);
    dot_text(dot, DOT_TITLE, all_seen ? "All seen" : "No videos here", 70, 366 - DY, DOT_TEXT, DOT_LEFT, 360);
    dot_text(dot, DOT_BODY_S, all_seen ? "Press SELECT to show everything." : "Put films and series in the Videos folder on the SD card.",
             70, 424 - DY, DOT_LABEL, DOT_LEFT, 360);
    return;
  }
  Item *it = &items[cursor];
  char path[1400], title[256], buf[64];
  item_path(cursor, path, sizeof path);
  title_of(it->name, title, sizeof title);
  draw_thumb_area(70, 126, path, it->dir);
  dot_text_scroll(dot, DOT_TITLE, it->dir ? it->name : title, 70, 366 - DY, DOT_TEXT, 360);   // too long: it scrolls
  if (it->virt && jf_offline()) {
    dot_text(dot, DOT_LABEL_T, "Server · no Wi-Fi", 70, 420 - DY, DOT_LABEL, DOT_LEFT, 360);
    dot_text(dot, DOT_BODY_S, "Jellyfin needs Wi-Fi. Turn it on in the device settings.", 70, 462 - DY, DOT_LABEL, DOT_LEFT, 360);
    return;
  }
  if (it->virt) {
    dot_text(dot, DOT_LABEL_T, jf_logged_in() ? "Server · logged in" : "Server · not connected", 70, 420 - DY, DOT_LABEL, DOT_LEFT, 360);
    if (jf_logged_in()) dot_text(dot, DOT_BODY_S, jf_user_name(), 70, 462 - DY, DOT_TEXT, DOT_LEFT, 360);
    else dot_text(dot, DOT_BODY_S, "Press A to connect with Quick Connect.", 70, 462 - DY, DOT_LABEL, DOT_LEFT, 360);
    return;
  }
  if (it->dir) {
    snprintf(buf, sizeof buf, folder_count == 1 ? "Folder · 1 item" : "Folder · %d items", folder_count);
    dot_text(dot, DOT_LABEL_T, buf, 70, 420 - DY, DOT_LABEL, DOT_LEFT, 360);
    return;
  }
  MediaInfo mi; int known = media_info(path, &mi);
  if (known) { media_label(&mi, buf, sizeof buf); dot_text(dot, DOT_LABEL_T, buf, 70, 420 - DY, DOT_LABEL, DOT_LEFT, 360); }
  double pos = 0, dur = known ? mi.duration : 0;
  int st = store_resume(path, &pos, NULL);
  Uint32 acc = !strcmp(thumb_for, path) ? thumb_accent : DOT_ACCENT_KINO;
  if (st == RES_PARTIAL) {
    dot_adot(dot, 70, 478 - DY, acc);
    dot_text(dot, DOT_LABEL_T, "Resume", 102, 478 - DY, DOT_TEXT, DOT_LEFT, 0);
    fmt_time(pos, buf, sizeof buf);
  } else {
    dot_text(dot, DOT_LABEL_T, st == RES_SEEN ? "Seen" : "Length", 70, 478 - DY, DOT_LABEL, DOT_LEFT, 0);
    if (dur > 0) fmt_time(dur, buf, sizeof buf); else snprintf(buf, sizeof buf, "--:--");
  }
  dot_time(dot, DOT_DISPLAY_L, buf, 68, 506 - DY, DOT_TEXT, DOT_LEFT);
  dot_progress(dot, 70, 578 - DY, 360, st == RES_SEEN ? 1 : st == RES_PARTIAL && dur > 0 ? (float)(pos / dur) : 0, 0);
}

static SDL_Texture *poster_tex(const char *url) {   // the cover's texture once the worker has it
  PosterTex *free_slot = &ptex[0];
  for (int i = 0; i < 32; i++) {
    if (ptex[i].t && !strcmp(ptex[i].url, url)) { ptex[i].used = ++ptick; return ptex[i].t; }
    if (!ptex[i].t || ptex[i].used < free_slot->used) free_slot = &ptex[i];
  }
  const uint8_t *rgb;
  if (!media_poster(url, &rgb)) return NULL;
  if (!free_slot->t) free_slot->t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, POSTER_W, POSTER_H);
  SDL_UpdateTexture(free_slot->t, NULL, rgb, POSTER_W * 3);
  snprintf(free_slot->url, sizeof free_slot->url, "%s", url);
  free_slot->used = ++ptick;
  return free_slot->t;
}

static void len_label(double secs, char *out, size_t n) {   // "1h 52m", "44 min"
  int m = (int)(secs / 60 + .5);
  if (m >= 60) snprintf(out, n, "%dh %02dm", m / 60, m % 60); else if (m) snprintf(out, n, "%d min", m); else out[0] = 0;
}

// One cover of the grid at x, y (focus: with its ring).
static void grid_cell(void *ud, int i, int x, int y, int focus) {
  (void)ud;
  char url[1100] = ""; float prog = 0; int seen = 0, fresh = 0;
  if (jf_mode) {
    JfItem *it = &jitems[i];
    jf_poster_url(it, POSTER_W, POSTER_H, url, sizeof url);
    prog = !it->folder && !it->played && it->pos > 0 && it->runtime > 0 ? (float)(it->pos / it->runtime) : 0;
    seen = it->played; fresh = it->folder && it->unplayed > 0;
  } else {
    snprintf(url, sizeof url, "%s", cover_of(i));
    if (!items[i].dir) {
      char p[1400]; double pos, dur; item_path(i, p, sizeof p);
      int st = store_resume(p, &pos, &dur);
      seen = st == RES_SEEN; if (st == RES_PARTIAL && dur > 0) prog = (float)(pos / dur);
    }
  }
  dot_poster(dot, *url ? poster_tex(url) : NULL, x, y, GPW, GPH, focus, prog, seen, fresh);
}

static int amb_on;   // the grid has its picture background (light header, night dock)
static void draw_grid(void) {   // 5 x 2 covers; the focused one's name in the header
  char label[96], lab[64], name[256];
  int folder, resumable = 0;
  if (jf_mode) {
    JfItem *f = &jitems[cursor];
    jf_label(f, lab, sizeof lab);
    snprintf(name, sizeof name, "%s", f->name);
    folder = f->folder; resumable = f->pos > 0 && !f->played;
  } else {
    Item *f = &items[cursor];
    char p[1400]; item_path(cursor, p, sizeof p);
    folder = f->dir;
    if (folder) { snprintf(name, sizeof name, "%s", f->name);
                  snprintf(lab, sizeof lab, folder_count == 1 ? "Folder · 1 item" : folder_count >= 0 ? "Folder · %d items" : "Folder", folder_count); }
    else {
      MediaInfo mi; lab[0] = 0;
      title_of(f->name, name, sizeof name);
      if (media_info(p, &mi)) {
        char res[32], len[24]; media_label(&mi, res, sizeof res); *strchr(res, ' ') = 0; len_label(mi.duration, len, sizeof len);
        snprintf(lab, sizeof lab, "%s%s%s", len, *len ? " · " : "", res);
      }
      resumable = store_resume(p, NULL, NULL) == RES_PARTIAL;
    }
  }
  snprintf(label, sizeof label, "%s%s%d / %d", lab, *lab ? " · " : "", cursor + 1, nitems);
  int lw = dot_text_w(dot, DOT_LABEL_T, label);
  // The covers move the Dot way (dot_grid): the rows slide, the focus ring slides, the next row peeks out.
  static DotGrid dg;
  unsigned key = (unsigned)jf_mode * 7u + (unsigned)jf_depth * 131u + (unsigned)nitems * 17u + (unsigned)settings.sort;
  for (const char *c = jf_mode ? jstack[jf_depth].name : cwd; *c; c++) key = key * 33u + (unsigned char)*c;
  dot_grid(dot, &dg, (int)key, nitems, GCOLS, 2, top, cursor, 40, 96, GPW, GPH, 16, DOT_PEEK_BELOW | DOT_PEEK_ABOVE, grid_cell, NULL);
  dot_text_scroll(dot, DOT_TITLE, name, 40, 22, DOT_ON_GROUND, CW - lw - 40);   // over the row before, peeking out
  dot_text(dot, DOT_LABEL_T, label, RIGHT, 30, DOT_ON_GROUND, DOT_RIGHT, 0);
  DotHint h[4]; int n = 0;
  if (folder) h[n++] = (DotHint){ "A", "Open" };
  else h[n++] = (DotHint){ "A", resumable ? "Resume" : "Play" };
  h[n++] = (DotHint){ "SELECT", "Sort" };
  h[n++] = (DotHint){ "B", jf_mode || depth ? "Back" : "Quit" };
  h[n++] = (DotHint){ "Y", "Options" };
  dot_set_night(dot, amb_on);   // over the picture: the dark glass dock with light text
  dot_dock(dot, h, n);
  dot_set_night(dot, 0);
}

// The grid's background: the focused cover, blurred and darkened, crossfading as the focus moves. Blurred cheaply:
// the cover is drawn into 44x66, that into 11x17, and that stretched over the screen with smooth scaling. A copy of
// the 11x17 stays in memory so posters' rounded corners can take the colour behind them.
#define AMB_W 11
#define AMB_H 17
#define AMB_DARK 150   // black over it (of 255): white text and the night dock stay readable
#define AMB_BW 176   // the smooth picture made from the 11x17 on the CPU (renderers that only scale blocky)
#define AMB_BH 272
static SDL_Texture *amb_mid, *amb[2], *amb_big[2];
static Uint32 amb_bigpx[AMB_BW * AMB_BH];
static Uint32 amb_px[2][AMB_W * AMB_H], amb_t0;
static int amb_cur, amb_n;   // amb_n: pictures made since the grid opened (0: none yet)
static char amb_key[1100];
// a 2:3 cover filling the width, its middle in the middle of the screen (y -444 on the Brick)
#define AMB_DST ((SDL_Rect){ -40, H / 2 - (W + 80) * 3 / 4, W + 80, (W + 80) * 3 / 2 })
static Uint32 amb_backdrop(float x, float y) {   // the darkened picture's colour at a screen point
  float u = (x - AMB_DST.x) / AMB_DST.w * AMB_W - .5f, v = (y - AMB_DST.y) / AMB_DST.h * AMB_H - .5f;
  int x0 = u < 0 ? 0 : u > AMB_W - 1 ? AMB_W - 1 : (int)u, y0 = v < 0 ? 0 : v > AMB_H - 1 ? AMB_H - 1 : (int)v;
  int x1 = x0 + 1 < AMB_W ? x0 + 1 : x0, y1 = y0 + 1 < AMB_H ? y0 + 1 : y0;
  float fx = u - x0, fy = v - y0; fx = fx < 0 ? 0 : fx > 1 ? 1 : fx; fy = fy < 0 ? 0 : fy > 1 ? 1 : fy;
  const Uint32 *px = amb_px[amb_cur]; Uint32 o = 0;
  for (int sh = 0; sh < 24; sh += 8) {
    float a = (px[y0 * AMB_W + x0] >> sh & 255) * (1 - fx) + (px[y0 * AMB_W + x1] >> sh & 255) * fx;
    float b = (px[y1 * AMB_W + x0] >> sh & 255) * (1 - fx) + (px[y1 * AMB_W + x1] >> sh & 255) * fx;
    o |= (Uint32)((a * (1 - fy) + b * fy) * (255 - AMB_DARK) / 255 + .5f) << sh;
  }
  return o;
}
static int amb_broken;   // the device can't make the textures: the plain ground, for good
static int ambient(const char *url) {   // draws the background; 0 when there is no picture yet (the ground then)
  SDL_Texture *src = *url && !amb_broken ? poster_tex(url) : NULL;
  if (src) {
    static Uint32 want_at; static char want[1100];   // holding the d-pad: rebuild only once the focus rests 120 ms
    if (strcmp(want, url)) { snprintf(want, sizeof want, "%s", url); want_at = SDL_GetTicks() + (amb_n && !shots_mode ? 120 : 0); }
    if (SDL_GetTicks() < want_at) src = NULL;
  }
  if (src && strcmp(amb_key, url)) {
    if (!amb_mid) {
      const char *was = SDL_GetHint(SDL_HINT_RENDER_SCALE_QUALITY); char keep[16]; snprintf(keep, sizeof keep, "%s", was ? was : "0");
      SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
      amb_mid = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 44, 66);
      for (int k = 0; k < 2; k++) {
        amb[k] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, AMB_W, AMB_H);
        amb_big[k] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, AMB_BW, AMB_BH);
      }
      SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, keep);
      if (!amb_mid || !amb[0] || !amb[1] || !amb_big[0] || !amb_big[1]) {   // all or nothing: no leak, no retry every cover
        SDL_Texture **all[5] = { &amb_mid, &amb[0], &amb[1], &amb_big[0], &amb_big[1] };
        for (int k = 0; k < 5; k++) if (*all[k]) { SDL_DestroyTexture(*all[k]); *all[k] = NULL; }
        amb_broken = 1;
      }
    }
    if (amb_mid && amb[0] && amb[1] && amb_big[0] && amb_big[1]) {
#if SDL_VERSION_ATLEAST(2, 0, 12)
      SDL_SetTextureScaleMode(src, SDL_ScaleModeLinear);
#endif
      int k = amb_n ? !amb_cur : amb_cur;
      SDL_SetRenderTarget(ren, amb_mid); SDL_RenderCopy(ren, src, NULL, NULL);
      SDL_SetRenderTarget(ren, amb[k]); SDL_RenderCopy(ren, amb_mid, NULL, NULL);
      SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, amb_px[k], AMB_W * 4);
      SDL_SetRenderTarget(ren, NULL);
      // 11x17 -> 66x102, bilinear with smoothstep between the samples: soft, no blocks
      for (int y = 0; y < AMB_BH; y++) for (int x = 0; x < AMB_BW; x++) {
        float u = (x + .5f) * AMB_W / AMB_BW - .5f, v = (y + .5f) * AMB_H / AMB_BH - .5f;
        int x0 = u < 0 ? 0 : (int)u, y0 = v < 0 ? 0 : (int)v, x1 = x0 + 1 < AMB_W ? x0 + 1 : x0, y1 = y0 + 1 < AMB_H ? y0 + 1 : y0;
        float fx = u - x0, fy = v - y0; fx = fx < 0 ? 0 : fx; fy = fy < 0 ? 0 : fy;
        fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
        const Uint32 *px = amb_px[k]; Uint32 o = 0xff000000;
        for (int sh = 0; sh < 24; sh += 8) {
          float a = (px[y0 * AMB_W + x0] >> sh & 255) * (1 - fx) + (px[y0 * AMB_W + x1] >> sh & 255) * fx;
          float b = (px[y1 * AMB_W + x0] >> sh & 255) * (1 - fx) + (px[y1 * AMB_W + x1] >> sh & 255) * fx;
          o |= (Uint32)(a * (1 - fy) + b * fy + .5f) << sh;
        }
        amb_bigpx[y * AMB_BW + x] = o;
      }
      SDL_UpdateTexture(amb_big[k], NULL, amb_bigpx, AMB_BW * 4);
      amb_cur = k; amb_n++; amb_t0 = SDL_GetTicks();
      snprintf(amb_key, sizeof amb_key, "%s", url);
    }
  }
  if (!amb_n) return 0;
  float t = shots_mode ? 1 : (SDL_GetTicks() - amb_t0) / 250.f; if (t > 1) t = 1;
  if (amb_n == 1) dot_ground(dot);   // the first picture fades in over the ground
  else { SDL_SetTextureAlphaMod(amb_big[!amb_cur], 255); SDL_RenderCopy(ren, amb_big[!amb_cur], NULL, &AMB_DST); }
  SDL_SetTextureBlendMode(amb_big[amb_cur], SDL_BLENDMODE_BLEND);
  SDL_SetTextureAlphaMod(amb_big[amb_cur], (Uint8)(255 * t));
  SDL_RenderCopy(ren, amb_big[amb_cur], NULL, &AMB_DST);
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(ren, 0, 0, 0, AMB_DARK);
  SDL_RenderFillRect(ren, NULL);
  SDL_Rect band = { 0, 0, W, 84 }; SDL_SetRenderDrawColor(ren, 0, 0, 0, 70); SDL_RenderFillRect(ren, &band);   // the header reads on any cover
  return 1;
}
static const char *focused_cover(char *buf, size_t n) {
  if (jf_mode) { jf_poster_url(&jitems[cursor], POSTER_W, POSTER_H, buf, n); return buf; }
  snprintf(buf, n, "%s", cover_of(cursor)); return buf;
}

// One row of the browser's list at y (sel: drawn focused, on the paper).
static void browse_row(void *ud, int i, int y, int sel) {
  int mixed = *(int *)ud;
  char title[256], meta[32] = "", path[1400];
  title_of(items[i].name, title, sizeof title);
  if (jf_mode) {
    JfItem *it = &jitems[i];
    snprintf(title, sizeof title, "%s", items[i].name);
    if (it->played) snprintf(meta, sizeof meta, "SEEN");
    else if (it->pos > 0 && it->runtime > 0) snprintf(meta, sizeof meta, "%d%%", (int)(100 * it->pos / it->runtime));
    else if (it->folder && it->unplayed) snprintf(meta, sizeof meta, "%d NEW", it->unplayed);
    else if (!it->folder && it->runtime > 0) fmt_time(it->runtime, meta, sizeof meta);
  } else if (!items[i].dir) {
    item_path(i, path, sizeof path);
    double pos, dur; MediaInfo mi;
    int st = store_resume(path, &pos, &dur);
    if (st == RES_SEEN) snprintf(meta, sizeof meta, "SEEN");
    else if (st == RES_PARTIAL && dur > 0) snprintf(meta, sizeof meta, "%d%%", (int)(100 * pos / dur));
    else if (media_info(path, &mi) && mi.duration > 0) fmt_time(mi.duration, meta, sizeof meta);
  }
  int off = items[i].virt && jf_offline();
  if (off) snprintf(meta, sizeof meta, "NO WI-FI");
  int icon = items[i].virt ? DOT_I_TV : jf_mode && !strcmp(jitems[i].type, "Special") ? DOT_I_PLAY : items[i].dir ? DOT_I_FOLDER : DOT_I_FILM;
  if (icon == DOT_I_FILM && !mixed) icon = DOT_I_NONE;   // only videos here: the icon says nothing, the title gets the room
  dot_row(dot, 476, y, 484 + DW, icon, items[i].dir && !jf_mode ? items[i].name : title, NULL, meta,
          (sel ? DOT_FOCUS : 0) | (off ? DOT_OFF : 0), 0);
}

static void draw_browse(void) {
  dot_set_ground(dot, settings.ground ? DOT_GROUND_TOPCENTRE : DOT_GROUND_TOPLEFT);
  dot_set_backdrop(dot, NULL);
  if (in_grid()) {
    char u[1100];
    amb_on = ambient(focused_cover(u, sizeof u));
    if (!amb_on) dot_ground(dot); else dot_set_backdrop(dot, amb_backdrop);
    draw_grid();
    dot_set_backdrop(dot, NULL);
    return;
  }
  if (jf_mode && nitems && !jf_loading && strcmp(jitems[cursor].type, "Special")) {   // Jellyfin lists: the same background
    char u[1100]; jf_poster_url(&jitems[cursor], POSTER_W, POSTER_H, u, sizeof u);
    amb_on = ambient(u);
    if (!amb_on) dot_ground(dot); else dot_set_backdrop(dot, amb_backdrop);
  } else {
    amb_key[0] = 0; amb_n = 0; amb_on = 0;
    dot_ground(dot);
  }
  // Crumb: "SD card / Videos / ...", only the last two folders when it's deeper than that.
  char crumb[700], count[48], rel[1100];
  snprintf(crumb, sizeof crumb, strcmp(root, home) ? "SD card" : "SD card / Videos");
  if (jf_mode) {   // "Jellyfin / Shows / Northern"
    snprintf(crumb, sizeof crumb, jf_depth > 2 ? "…" : "Jellyfin");
    for (int i = jf_depth > 2 ? jf_depth - 1 : 1; i <= jf_depth; i++) {
      size_t l = strlen(crumb); snprintf(crumb + l, sizeof crumb - l, " / %s", jstack[i].name);
    }
    rel[0] = 0;
  } else
  snprintf(rel, sizeof rel, "%s", cwd + strlen(root));
  char *part[MAXDEPTH]; int np = 0;
  for (char *t = strtok(rel, "/"); t && np < MAXDEPTH; t = strtok(NULL, "/")) part[np++] = t;
  if (np > 2) snprintf(crumb, sizeof crumb, "…");
  for (int i = np > 2 ? np - 2 : 0; i < np; i++) {
    size_t l = strlen(crumb); snprintf(crumb + l, sizeof crumb - l, " / %s", part[i]);
  }
  if (nitems > ROWS) snprintf(count, sizeof count, "%d–%d of %d", top + 1, top + ROWS < nitems ? top + ROWS : nitems, nitems);
  else snprintf(count, sizeof count, nitems == 1 ? "1 item" : "%d items", nitems);
  draw_detail();
  // The rows move the Dot way (dot_list): they slide when the list scrolls, the paper slides to the focused row, and
  // the next row's top peeks out under the last when there's more.
  int mixed = 0;   // folders (or the Jellyfin row) among the videos: keep the film icon to tell them apart
  for (int i = 0; i < nitems && !mixed; i++) mixed = items[i].dir || items[i].virt;
  static DotList dl;
  unsigned key = (unsigned)jf_mode * 7u + (unsigned)jf_depth * 131u + (unsigned)nitems;   // which list: where, how long
  for (const char *c = jf_mode ? jstack[jf_depth].name : cwd; *c; c++) key = key * 33u + (unsigned char)*c;
  dot_list(dot, &dl, (int)key, nitems, ROWS, top, cursor, 476, 484 + DW, DOT_PEEK_BELOW | DOT_PEEK_ABOVE, browse_row, &mixed);
  dot_header(dot, crumb, count);   // over the row before, peeking out under it
  DotHint h[4]; int n = 0;
  if (jf_mode) {
    if (nitems && !jf_loading) {
      JfItem *it = &jitems[cursor];
      if (it->folder) h[n++] = (DotHint){ "A", "Open" };
      else if (it->pos > 0 && !it->played) { h[n++] = (DotHint){ "A", "Resume" }; h[n++] = (DotHint){ "X", "From start" }; }
      else h[n++] = (DotHint){ "A", "Play" };
    }
    if (jf_grid && !jf_loading && !nitems) h[n++] = (DotHint){ "SELECT", "Sort" };
    h[n++] = (DotHint){ "B", "Back" };
    h[n++] = (DotHint){ "Y", "Options" };
    dot_dock(dot, h, n);
    return;
  }
  if (nitems && items[cursor].virt && jf_offline()) ;   // greyed out: nothing to open
  else if (nitems && items[cursor].dir) h[n++] = (DotHint){ "A", "Open" };
  else if (nitems) {
    char path[1400]; item_path(cursor, path, sizeof path);
    if (store_resume(path, NULL, NULL) == RES_PARTIAL) { h[n++] = (DotHint){ "A", "Resume" }; h[n++] = (DotHint){ "X", "From start" }; }
    else h[n++] = (DotHint){ "A", "Play" };
  }
  if (local_grid && !nitems) h[n++] = (DotHint){ "SELECT", "Sort" };
  h[n++] = (DotHint){ "B", depth ? "Back" : "Quit" };
  h[n++] = (DotHint){ "Y", "Options" };
  dot_dock(dot, h, n);
}

static SDL_Rect video_dst;   // where the picture is on the screen, for the subtitles' place
static void draw_video(void) {
  SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
  SDL_RenderClear(ren);
  SDL_Rect s, d;
  SDL_Texture *t = player_texture(&s, &d);
  if (t) { SDL_RenderCopy(ren, t, &s, &d); video_dst = d; }
  if (t && settings.filter == FILTER_CRT) {   // scanlines every third row and a soft vignette, made once
    if (!crt_tex) {
      Uint32 *px = malloc((size_t)W * H * 4);
      if (!px) return;
      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
          float dx = (x - W / 2.f) / (W / 2.f), dy = (y - H / 2.f) / (H / 2.f), v = (dx * dx + dy * dy) * .5f;
          int a = (int)(v * v * 255 * .55f) + (y % 3 == 2 ? 110 : 0);
          px[y * W + x] = (Uint32)(a > 255 ? 255 : a) << 24;
        }
      crt_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, W, H);
      SDL_UpdateTexture(crt_tex, NULL, px, W * 4); free(px);
      SDL_SetTextureBlendMode(crt_tex, SDL_BLENDMODE_BLEND);
    }
    SDL_Rect c = d;
    if (SDL_IntersectRect(&d, &(SDL_Rect){ 0, 0, W, H }, &c)) SDL_RenderCopy(ren, crt_tex, &c, &c);
  }
}

// The controls: a band along the bottom edge, full width and square (Dot's dot_band), from y 500 on the Brick (48
// higher on the Smart Pro). It slides up and down (dot_slide; e: how far up, 0-1) and what's on it moves with it; the
// pills at the top fade with it.
#define BAND_Y (500 - DY)
static int band_top = 768;   // where the band's top is now (the screen's height when it's down): the toast goes above it
static void draw_overlay(float e) {
  dot_set_accent(dot, player_accent());
  char buf[64], dur[32];
  int off = (int)lroundf((1 - e) * (H - BAND_Y));   // how far below its place the band is
  band_top = BAND_Y + off;
  dot_set_alpha(dot, e);
  dot_pill(dot, 40, 36, playing_title, 1, 0, DOT_LEFT);
  MediaInfo mi;
  if (jf_playing) dot_pill(dot, RIGHT, 36, jf_transcoded ? "Jellyfin · converted" : "Jellyfin · direct", 0, 1, DOT_RIGHT);
  else if (media_info(playing, &mi)) { media_audio_label(&mi, buf, sizeof buf); dot_pill(dot, RIGHT, 36, buf, 0, 1, DOT_RIGHT); }
  dot_set_alpha(dot, 1);
  if (off >= H - BAND_Y) return;
  double pos = player_pos(), len = player_duration();
  int paused = player_paused();
  if (on_bar) pos = scrub_to;
  dot_band(dot, band_top, H - band_top);
  int dy = DY - off;   // the band's contents: at their places, moved down as far as the band is
  fmt_time(pos, buf, sizeof buf);
  int tw = dot_time(dot, DOT_DISPLAY_XL, buf, 76, 526 - dy, DOT_TEXT, DOT_LEFT);
  int lx = 76 + tw + 30;
  fmt_time(len, dur, sizeof dur); snprintf(buf, sizeof buf, "/ %s", dur);
  dot_text(dot, DOT_LABEL_T, buf, lx, 546 - dy, DOT_LABEL, DOT_LEFT, 0);
  dot_adot(dot, lx, 588 - dy, dot_accent(dot));
  dot_text(dot, DOT_LABEL_T, paused ? "Paused" : "Playing", lx + 32, 584 - dy, DOT_TEXT, DOT_LEFT, 0);
  int seeking = !on_bar && SDL_GetTicks() < seek_until ? seek_dir : 0;   // the pressed side is lit, with the ring
  dot_round(dot, 702 + DW, 570 - dy, DOT_I_BACK, 0, seeking < 0 ? DOT_FOCUS | DOT_ON : 0);
  dot_round(dot, 806 + DW, 570 - dy, paused ? DOT_I_PLAY : DOT_I_PAUSE, 1, seeking || on_bar ? 0 : DOT_FOCUS);
  dot_round(dot, 900 + DW, 570 - dy, DOT_I_FWD, 0, seeking > 0 ? DOT_FOCUS | DOT_ON : 0);
  dot_progress(dot, 76, 644 - dy, CW - 72, len > 0 ? (float)(pos / len) : 0, on_bar ? 2 : 1);
  // the buttons: the longest way of putting it that fits (the Brick's narrower band gets the short one)
  static const char *const HINTS[] = { "◀ ▶ 10 s · ▲ audio, subs · ▼ scrub · L1 R1 filter · L2 R2 aspect",
                                       "◀ ▶ 10 s · ▲ audio, subs · ▼ scrub · L1 R1 L2 R2 picture",
                                       "◀ ▶ 10 s · ▲ tracks · ▼ scrub · L1 R1 L2 R2 picture" };
  const char *hint = on_bar ? "◀ ▶ move · L1 R1 faster · A jump here · B cancel" : HINTS[0];
  for (int i = 1; !on_bar && i < 3 && dot_text_w(dot, DOT_PILL_MONO, hint) > CW - 72; i++) hint = HINTS[i];
  dot_text(dot, DOT_PILL_MONO, hint, 76, 690 - dy, DOT_LABEL, DOT_LEFT, CW - 72);
}

// A film that broke off (or a Jellyfin one stuck waiting): try again in 2, 4, 6, 8, 10 s, asking the server for a fresh
// stream each time; after that give up, back to the list, the place kept, not marked as seen.
static void stream_lost(const char *how) {
  if (jf_playing && lost_tries < LOST_TRIES) {
    lost_tries++; retry_at = SDL_GetTicks() + 2000 * lost_tries;
    net_log(lost_tries == 1 ? "lost" : NULL, "stream %s at %.0f s, reconnecting %d/%d in %d s", how, player_pos(), lost_tries,
            LOST_TRIES, 2 * lost_tries);
    return;
  }
  int net = jf_playing;   // stop() clears it
  stop(0);
  show_toast(net ? "Lost the connection to Jellyfin" : "Couldn't play this file");
  if (net) net_log("gave-up", "gave up after %d tries: toast: Lost the connection to Jellyfin", LOST_TRIES);
}

// Watches a Jellyfin film for a network that can't keep up: the second time it stops to buffer, or the Wi-Fi link
// drops (roaming between mesh points, lost router) even with a good signal. Says so once, with what to try.
static void net_watch(void) {
  int b = player_buffering(), buffering = b >= 0 && b <= 100;   // 101 is opening or seeking: not a stall
  if (buffering && !net_was_buffering && player_pos() > playing_jf.pos + 5) {
    char w[120]; wifi_describe(w, sizeof w);
    net_stalls++; net_log(NULL, "stall %d at %.0f s (buffer %d%%), %s", net_stalls, player_pos(), b, w);
  } else if (!buffering && net_was_buffering) net_log(NULL, "playing again at %.0f s", player_pos());
  net_was_buffering = buffering;
  static int drops_seen; int d = wifi_drops() - net_drops0;
  if (d > 0 && d != drops_seen) net_log(NULL, "Wi-Fi link dropped (%d this film)", d);
  drops_seen = d;
  if (net_poor) return;
  int dropped = d > 0;
  if (net_stalls < 2 && !dropped && !lost_tries) return;
  net_poor = 1;
  char t[160];
  snprintf(t, sizeof t, "%s · %s", dropped || lost_tries ? "Wi-Fi keeps dropping out" : "Wi-Fi too slow for this film",
           settings.quality > QUALITY_2 ? "try a lower quality in Options" : "move closer to the router");
  show_toast(t); toast_until = SDL_GetTicks() + 7000;
  net_log("warning", "toast: %s", t);
}

// Waiting, as a Dot busy card in the middle (a ring of dots running round): a Jellyfin film starting, the sound being
// changed, reconnecting, or buffering (with how far, and the Wi-Fi when it's weak). Opening or seeking shows only after
// 600 ms, so a quick seek doesn't flash it. Returns 0 when nothing is being waited for.
static int busy_shown;   // the card is up or still fading out: the main loop keeps drawing
static int busy_now(char *label, size_t ln, char *value, size_t vn) {
  static Uint32 wait_since;
  int b = player_active() ? player_buffering() : -1;
  if (b < 0 || screen != SCR_PLAY) wait_since = 0;
  else if (!wait_since) wait_since = SDL_GetTicks();
  if (!player_active() && !job_th) opening = 0;
  if (screen == SCR_PLAY && b < 0 && !job_th) opening = 0;   // playing
  const char *wifi = wifi_state() == WIFI_OFF ? "No Wi-Fi" : wifi_state() == WIFI_WEAK ? "Weak Wi-Fi" :
                     net_poor ? "Wi-Fi too slow" : NULL;
  snprintf(value, vn, "%s", jf_playing && wifi ? wifi : playing_title);
  if (lost_tries) { snprintf(label, ln, "RECONNECTING %d OF %d", lost_tries, LOST_TRIES); return 1; }
  if (jf_playing && job_th && job_kind == J_PLAY) {
    if (sound_change) { snprintf(label, ln, "CHANGING THE SOUND"); return 1; }
    snprintf(label, ln, "STARTING"); return 1;
  }
  if (opening && jf_playing && b > 100) { snprintf(label, ln, "STARTING"); return 1; }   // on from the line above, no gap
  if (b < 0 || SDL_GetTicks() - wait_since < 600) return 0;
  if (b > 100) snprintf(label, ln, opening ? "STARTING" : jf_playing ? "WAITING FOR THE SERVER" : "LOADING");
  else snprintf(label, ln, "BUFFERING %d%%", b);
  return 1;
}
static void draw_busy(void) {
  static DotSlide sl; static char l[48], v[160];
  char nl[48], nv[160];
  int show = busy_now(nl, sizeof nl, nv, sizeof nv);
  if (show) { snprintf(l, sizeof l, "%s", nl); snprintf(v, sizeof v, "%s", nv); }   // what fades out is what was up
  dot_busy(dot, &sl, show, DOT_I_FILM, l, v, 1);
  busy_shown = show || sl.v > 0;
}

static const char *DEINTS[NDEINT] = { "AUTO", "ON", "OFF" };

// Tabs at the top right: Dot's switch, its paper sliding to the active one (L1/R1; the dock says so). Equal parts, as
// wide as the longest name needs.
static void draw_tabs(DotSwitch *sw, const char *const *names, int n, int active) {
  int seg = 0;
  for (int i = 0; i < n; i++) { int w = dot_text_w(dot, DOT_CHIP_T, names[i]) + 48; if (w > seg) seg = w; }
  dot_switch(dot, sw, DOT_W - DOT_MARGIN - seg * n, 28, seg * n, names, n, active, 0);
}

// The sound track. A stream the server converts carries one track: Kino lists the film's tracks from the server and
// switches by asking for the stream again with the other one, from where it is (a second or two).
static int jf_audio_mode(void) { return jf_playing && jf_converting() && jf_audio_count() > 0; }
static const char *audio_label(void) {
  if (!jf_audio_mode()) return player_audio_label();
  static char lab[40]; int n = jf_audio_count(), idx; const char *code;
  for (int i = 0; jf_audio(i, &idx, &code); i++) if (idx == jf_audio_now()) {
    const char *name = lang_label(code);
    if (name) snprintf(lab, sizeof lab, "%s", name); else snprintf(lab, sizeof lab, "TRACK %d", i + 1);
    if (n > 1) { size_t l = strlen(lab); snprintf(lab + l, sizeof lab - l, " · %d/%d", i + 1, n); }
    return lab;
  }
  return "AUTO";
}
static int audio_count(void) { return jf_audio_mode() ? jf_audio_count() : player_audio_tracks(); }
// The sound tracks by name ("NORSK"), and the one in use: the film's own, or the server's list for a converted stream.
static int audio_items(char names[][40], int max, int *now) {
  if (!jf_audio_mode()) return player_audio_list(names, max, now);
  int n = 0, idx; const char *code; *now = -1;
  for (; n < max && jf_audio(n, &idx, &code); n++) {
    const char *name = lang_label(code);
    if (name) snprintf(names[n], 40, "%s", name); else snprintf(names[n], 40, "TRACK %d", n + 1);
    if (idx == jf_audio_now()) *now = n;
  }
  return n;
}
static void audio_pick(int i) {   // a converted stream: asked for again with that track, from where it is
  if (!jf_audio_mode()) { player_set_audio(i); return; }
  int idx; const char *code;
  if (job_th || !jf_audio(i, &idx, &code) || idx == jf_audio_now()) return;
  jf_audio_want = idx;   // kept for this film, also through reconnects
  playing_jf.pos = player_pos(); jf_play_from_start = 0;
  sound_change = 1;   // a busy card while it asks (busy_now)
  start_job(J_PLAY);
}
static void audio_switch(int dir) {
  if (!jf_audio_mode()) { if (player_active()) player_cycle_audio(dir); return; }
  char names[16][40]; int now, n = audio_items(names, 16, &now);
  if (n < 2) return;
  audio_pick(((now < 0 ? 0 : now) + dir + n) % n);
}

// The picker (UP while playing): sound and subtitles in the middle, Dot's picker with Audio | Subtitles on top when
// there's a choice of both. UP/DOWN picks at once (a converted Jellyfin stream: once the focus has rested 700 ms, as
// it's asked for again), LEFT/RIGHT flips the switch, A or B closes.
enum { PICK_AUDIO, PICK_SUBS };
static int picker_on, picker_shown, picker_tabs[2], picker_ntabs, picker_tab, picker_cur;
static int pick_wait = -1; static Uint32 pick_at;   // a converted stream's track waiting to be asked for
static int picker_items(int kind, char names[][40], int max, int *now) {
  if (kind == PICK_AUDIO) return audio_items(names, max, now);
  int sn, n = player_sub_list(names + 1, max - 1, &sn);
  snprintf(names[0], 40, "OFF");
  *now = settings.subs && sn >= 0 ? sn + 1 : 0;
  return n + 1;
}
static void picker_open(void) {
  char names[33][40]; int now;
  picker_ntabs = 0;
  if (audio_items(names, 33, &now) > 1) picker_tabs[picker_ntabs++] = PICK_AUDIO;
  if (player_sub_list(names, 33, &now) > 0) picker_tabs[picker_ntabs++] = PICK_SUBS;
  if (!picker_ntabs) { show_notice(DOT_I_NOTE, "SOUND AND SUBTITLES", "Nothing to choose", 0, 0); return; }
  picker_tab = 0; picker_on = 1; overlay_until = 0; on_bar = 0;
  picker_items(picker_tabs[0], names, 33, &picker_cur);
  if (picker_cur < 0) picker_cur = 0;
}
static void picker_button(int b) {
  char names[33][40]; int now, kind = picker_tabs[picker_tab];
  int n = picker_items(kind, names, 33, &now);
  if (b == BTN_A || b == BTN_B || b == BTN_Y) { picker_on = 0; return; }
  if ((b == BTN_LEFT || b == BTN_RIGHT) && picker_ntabs > 1) {
    picker_tab = !picker_tab;
    picker_items(picker_tabs[picker_tab], names, 33, &picker_cur);
    if (picker_cur < 0) picker_cur = 0;
    return;
  }
  if ((b != BTN_UP && b != BTN_DOWN) || n < 1) return;
  picker_cur = (picker_cur + (b == BTN_UP ? -1 : 1) + n) % n;
  if (kind == PICK_AUDIO) {
    if (jf_audio_mode()) { pick_wait = picker_cur; pick_at = SDL_GetTicks(); }
    else audio_pick(picker_cur);
  } else {
    settings.subs = picker_cur > 0;
    if (picker_cur > 0) player_set_sub(picker_cur - 1);
    settings_save(); player_apply_settings();
  }
}
static void draw_picker(void) {
  static DotPicker pk;
  static const char *const TABS[2] = { "Audio", "Subtitles" };
  if (pick_wait >= 0 && SDL_GetTicks() - pick_at >= 700 && !job_th) { audio_pick(pick_wait); pick_wait = -1; }
  if (picker_on && (screen != SCR_PLAY || !player_active())) picker_on = 0;
  static char names[33][40]; static const char *items[33]; static int n, now;   // kept: what fades out is what was up
  const char *tabs[2]; int kind = picker_tabs[picker_tab];
  for (int i = 0; i < picker_ntabs; i++) tabs[i] = TABS[picker_tabs[i]];
  if (picker_on) {
    n = picker_items(kind, names, 33, &now);
    for (int i = 0; i < n; i++) items[i] = names[i];
    if (kind == PICK_AUDIO && pick_wait >= 0) now = -1;   // not in use yet
  }
  dot_picker(dot, &pk, picker_on, tabs, picker_ntabs, picker_tab, items, n, picker_cur, now);
  picker_shown = picker_on || pk.slide.v > 0;
}

static void draw_options(void) {
  int night = opt_back == SCR_PLAY;
  if (night) { draw_video(); dot_scrim(dot); }
  else { dot_set_ground(dot, settings.ground ? DOT_GROUND_TOPCENTRE : DOT_GROUND_TOPLEFT); dot_ground(dot); }
  dot_set_night(dot, night);
  dot_title(dot, "OPTIONS");
  int f[NOPT]; for (int i = 0; i < NOPT; i++) f[i] = opt_sel == i ? DOT_FOCUS : 0;
  // Tabs (L1/R1) at the top right, the active one lit; one full-width card below with the tab's rows.
  int tab = opt_tab(opt_sel);
  static DotSwitch sw; draw_tabs(&sw, TAB_NAMES, NTABS, tab);
  int P = DOT_OPT_PITCH, x = 70, cw = CW - 60, y;
  if (tab == 0) {
    y = dot_panel(dot, 40, 108, CW, dot_panel_h(DOT_CHIP_H + 24 + 4 * P - 8) - 46, NULL);
    dot_chips(dot, x, y + 4, cw, ASPECT_NAMES, NASPECT, settings.aspect, opt_sel == O_ASPECT ? settings.aspect : -1);
    y += DOT_CHIP_H + 24;
    dot_opt_slider(dot, x, y, cw, "Brightness", (settings.brightness + 50) / 100.f, f[O_BRIGHT]);
    dot_opt_slider(dot, x, y + P, cw, "Contrast", (settings.contrast + 50) / 100.f, f[O_CONTRAST]);
    dot_opt(dot, x, y + 2 * P, cw, "Deinterlace", DEINTS[settings.deint], f[O_DEINT]);
    char fname[32]; snprintf(fname, sizeof fname, "%s", FILTER_NAMES[settings.filter]);
    for (char *q = fname; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
    dot_opt(dot, x, y + 3 * P, cw, "Filter", fname, f[O_FILTER]);
  } else if (tab == 1) {   // sound: the track now, and the languages Kino picks it by (as for subtitles)
    y = dot_panel(dot, 40, 108, CW, dot_panel_h(3 * P - 8) - 46, NULL);
    const char *a1 = lang_label(settings.aprefer1), *a2 = lang_label(settings.aprefer2);
    dot_opt(dot, x, y, cw, "Track", audio_label(), f[O_ATRACK] | (audio_count() > 1 ? 0 : DOT_OFF));
    dot_opt(dot, x, y + P, cw, "Prefer", a1 ? a1 : "FILM'S OWN", f[O_APREFER]);
    dot_opt(dot, x, y + 2 * P, cw, "Then", a2 ? a2 : "NONE", f[O_APREFER2] | (*settings.aprefer1 ? 0 : DOT_OFF));
  } else if (tab == 2) {
    y = dot_panel(dot, 40, 108, CW, dot_panel_h(4 * P - 8) - 46, NULL);
    // Show and size in one row: OFF, S, M, L, XL (A: on/off, keeping the size; SELECT while playing does the same)
    char show[24]; snprintf(show, sizeof show, "%s", settings.subs ? SUBSIZE_NAMES[settings.subsize] : "OFF");
    dot_opt(dot, x, y, cw, "Show · size", show, f[O_SUBS]);
    dot_opt(dot, x, y + P, cw, "Track", player_sub_label(), f[O_TRACK] | (player_active() ? 0 : DOT_OFF));
    // Languages: the first the film has wins; Prefer "Film's own" leaves it to the film (Then is unused then)
    const char *l1 = lang_label(settings.prefer1), *l2 = lang_label(settings.prefer2);
    dot_opt(dot, x, y + 2 * P, cw, "Prefer", l1 ? l1 : "FILM'S OWN", f[O_PREFER]);
    dot_opt(dot, x, y + 3 * P, cw, "Then", l2 ? l2 : "NONE", f[O_PREFER2] | (*settings.prefer1 ? 0 : DOT_OFF));
  } else {
    y = dot_panel(dot, 40, 108, CW, dot_panel_h(3 * P - 8) - 46, NULL);
    dot_opt_toggle(dot, x, y, cw, "Hardware decoding", settings.hwdec, f[O_HWDEC]);
    dot_opt(dot, x, y + P, cw, "Jellyfin quality", QUALITY_NAMES[settings.quality], f[O_QUALITY]);
    dot_opt_toggle(dot, x, y + 2 * P, cw, "Play next episode", settings.autonext, f[O_AUTONEXT]);
  }
  DotHint h[] = { { "<>", "Change" }, { "L1 R1", "Section" }, { "X", "Buttons" }, { "B", "Close" } };   // A changes too
  dot_dock(dot, h, 4);
  dot_set_night(dot, 0);
}

static void draw_connect(void) {   // Quick Connect: the code in dots, approved from the Jellyfin app
  dot_set_ground(dot, settings.ground ? DOT_GROUND_TOPCENTRE : DOT_GROUND_TOPLEFT);
  dot_ground(dot);
  dot_title(dot, "JELLYFIN");
  int y = dot_panel(dot, 40, 96, CW, 440, "Quick Connect");
  if (jf_code[0]) {
    char spaced[16]; snprintf(spaced, sizeof spaced, "%.3s %.3s", jf_code, jf_code + 3);
    dot_text(dot, DOT_DISPLAY_XL, spaced, 70, y + 20, DOT_TEXT, DOT_LEFT, 0);
    dot_text(dot, DOT_BODY_S, "In Jellyfin on your phone or computer, open your profile,", 70, y + 140, DOT_TEXT, DOT_LEFT, CW - 60);
    dot_text(dot, DOT_BODY_S, "choose Quick Connect and enter this code.", 70, y + 178, DOT_TEXT, DOT_LEFT, CW - 60);
    dot_text(dot, DOT_BODY_S, "Kino logs in by itself.", 70, y + 216, DOT_LABEL, DOT_LEFT, CW - 60);
    dot_adot(dot, 70, y + 287, DOT_ACCENT_KINO);
    dot_text(dot, DOT_LABEL_T, "Waiting", 102, y + 278, DOT_TEXT, DOT_LEFT, 0);
    dot_text(dot, DOT_LABEL_T, connect_msg, 70, y + 322, DOT_LABEL, DOT_LEFT, CW - 60);
  } else {
    dot_text(dot, DOT_TITLE, connect_msg, 70, y + 30, DOT_TEXT, DOT_LEFT, CW - 60);
  }
  DotHint h[2]; int n = 0;
  if (!jf_code[0] && !job_th) h[n++] = (DotHint){ "A", "Try again" };
  h[n++] = (DotHint){ "B", "Back" };
  dot_dock(dot, h, n);
}

// The buttons, shown on the first start and from Options (X).
static const DotHint HELP_BROWSE[] = { { "A", "Open, play, resume" }, { "X", "Play from the start" }, { "B", "Back" },
  { "L1 R1", "A page up or down" }, { "SELECT", "Sort the covers" }, { "Y", "Options" }, { "MENU", "Quit" } };
static const DotHint HELP_PLAY[] = { { "A", "Pause" }, { "<>", "10 s back, on" }, { "UP", "Audio, subtitles" }, { "DOWN", "Scrub, A jumps" },
  { "L1 R1", "Picture filter" }, { "L2 R2", "Aspect" }, { "SELECT", "Subtitles on, off" }, { "B", "Stop, keep place" } };   // two columns: short

static int help_tab;   // 0 browsing, 1 playing (L1/R1)
static void draw_next(void) {   // Up next: the episode's picture and names, and a countdown in dots
  dot_set_ground(dot, settings.ground ? DOT_GROUND_TOPCENTRE : DOT_GROUND_TOPLEFT);
  dot_ground(dot);
  dot_title(dot, "UP NEXT");
  char series[256] = "", line[200] = "", name[256] = "";
  if (next_kind == 2) {
    snprintf(series, sizeof series, "%s", next_jf.series);
    if (next_jf.index) snprintf(line, sizeof line, "Season %d · Episode %d", next_jf.season, next_jf.index);
    snprintf(name, sizeof name, "%s", next_jf.name);
  } else if (next_kind == 1 && next_local < nitems) {
    const char *d = strrchr(cwd, '/'); snprintf(series, sizeof series, "%.255s", d ? d + 1 : cwd);
    title_of(items[next_local].name, name, sizeof name);
    MediaInfo mi; if (media_info(next_pic, &mi) && mi.duration > 0) { char t[24]; fmt_time(mi.duration, t, sizeof t); snprintf(line, sizeof line, "Length %s", t); }
  }
  // The card comes in and goes the Dot way (dot_rise): the screen waits for it to go (next_leave, in the main loop).
  static Uint32 came, seen;   // when it came up; when it was last drawn (away for a moment: comes in again)
  Uint32 t = SDL_GetTicks();
  if (!seen || t - seen > 250) came = t;
  seen = t;
  int r = dot_rise(dot, (t - came) / (float)DOT_IN_MS, next_leave ? (t - next_leave) / (float)DOT_OUT_MS : 0);
  dot_card(dot, 40, 96 + r, CW, 528 - DY);
  draw_thumb_area(70, 126 + r, next_pic, 0);
  int tx = 70 + CARD_PIC_W + 32, tw = RIGHT - 30 - tx;
  dot_text(dot, DOT_LABEL_T, series, tx, 132 + r, DOT_LABEL, DOT_LEFT, tw);
  dot_text_scroll(dot, DOT_TITLE, name, tx, 172 + r, DOT_TEXT, tw);
  if (*line) dot_text(dot, DOT_BODY_S, line, tx, 226 + r, DOT_LABEL, DOT_LEFT, tw);
  Uint32 now = SDL_GetTicks(); int left = next_at > now ? (int)((next_at - now + 999) / 1000) : 0;
  dot_adot(dot, 70, 384 - DY + r, dot_accent(dot));
  dot_text(dot, DOT_LABEL_T, "Playing in", 102, 378 - DY + r, DOT_TEXT, DOT_LEFT, 0);
  char sec[16]; snprintf(sec, sizeof sec, "0:%02d", left);
  dot_time(dot, DOT_DISPLAY_XL, sec, 68, 420 - DY + r, DOT_TEXT, DOT_LEFT);
  float done = next_at > now ? 1 - (float)(next_at - now) / NEXT_MS : 1;
  dot_progress(dot, 70, 574 - DY + r, CW - 60, done, 0);
  dot_set_alpha(dot, 1);
  DotHint h[] = { { "A", "Play now" }, { "B", "Not now" } };
  dot_dock(dot, h, 2);
}

// One column of keys on the Brick; two side by side where one doesn't fit the height (the Smart Pro).
static void draw_help_panel(const DotHint *h, int n) {
  enum { PITCH = 68 };   // a key cap is 62 high
  int cols = 108 + dot_panel_h(n * PITCH - 6) - 46 > DOT_DOCK_Y - 38 ? 2 : 1, per = (n + cols - 1) / cols, colw = CW / cols;
  int y0 = dot_panel(dot, 40, 108, CW, dot_panel_h(per * PITCH - 6) - 46, NULL);
  for (int i = 0; i < n; i++) {
    int x = 70 + (i / per) * colw, y = y0 + (i % per) * PITCH;
    dot_key(dot, x, y, h[i].key, 0, 2);   // raised: on a black card
    dot_text(dot, DOT_UI, h[i].text, x + 170, y + (DOT_KEY - dot_line_h(dot, DOT_UI)) / 2, DOT_TEXT, DOT_LEFT, colw - 60 - 170);
  }
}

static void draw_help(void) {
  dot_set_ground(dot, settings.ground ? DOT_GROUND_TOPCENTRE : DOT_GROUND_TOPLEFT);
  dot_ground(dot);
  dot_title(dot, "BUTTONS");
  static const char *const names[2] = { "Browsing", "Playing" };
  static DotSwitch sw; draw_tabs(&sw, names, 2, help_tab);
  if (help_tab) draw_help_panel(HELP_PLAY, sizeof HELP_PLAY / sizeof *HELP_PLAY); else draw_help_panel(HELP_BROWSE, sizeof HELP_BROWSE / sizeof *HELP_BROWSE);
  DotHint h[2] = { help_back == SCR_OPTIONS ? (DotHint){ "B", "Close" } : (DotHint){ "A", "Got it" },
                   { "L1 R1", help_tab ? "Browsing" : "Playing" } };
  dot_dock(dot, h, 2);
}

// Skip: while Jellyfin's marked intro, recap, preview or end credits play, a button at the bottom right; X jumps past
// it. The credits of an episode go to Up next instead (when that's on).
static int skip_now(const char **label, double *to, int *credits) {
  if (!jf_playing || !player_active() || on_bar) return 0;
  double pos = player_pos(), s, e; const char *type;
  for (int i = 0; jf_segment(i, &type, &s, &e); i++) {
    if (pos < s || pos >= e - 2) continue;
    *to = e; *credits = !strcmp(type, "Outro");
    if (!strcmp(type, "Intro")) *label = "Skip intro";
    else if (!strcmp(type, "Recap")) *label = "Skip recap";
    else if (!strcmp(type, "Preview")) *label = "Skip preview";
    else if (*credits) *label = settings.autonext && !strcmp(playing_jf.type, "Episode") ? "Next episode" : "Skip credits";
    else continue;
    return 1;
  }
  return 0;
}
static void draw_skip(float e) {   // e: how far up the band is (the button rides above it)
  const char *label; double to; int credits;
  if (!skip_now(&label, &to, &credits)) return;
  int tw = dot_text_w(dot, DOT_UI, label), w = 16 + DOT_KEY + 14 + tw + 26, h = 80, x = RIGHT - w,
      y = 652 - DY - (int)lroundf(248 * e);
  dot_rect(dot, x, y, w, h, DOT_R_CARD, DOT_GLASS, 225);
  dot_key(dot, x + 16, y + (h - DOT_KEY) / 2, "X", 1, 0);
  dot_text(dot, DOT_UI, label, x + 16 + DOT_KEY + 14, y + (h - dot_line_h(dot, DOT_UI)) / 2, DOT_TEXT, DOT_LEFT, 0);
}

static void draw(void) {
  dot_frame(dot);
  dot_set_accent(dot, DOT_ACCENT_KINO);
  dot_set_backdrop(dot, NULL);   // only the browser sets one, for its own frame
  Uint32 now = SDL_GetTicks();
  if (screen == SCR_BROWSE) draw_browse();
  else if (screen == SCR_CONNECT) draw_connect();
  else if (screen == SCR_OPTIONS) draw_options();
  else if (screen == SCR_HELP) draw_help();
  else if (screen == SCR_NEXT) draw_next();
  else {
    draw_video();
    int controls = (player_paused() || on_bar || now < overlay_until) && !picker_on;   // the picker: alone in the middle
    static DotSlide band;
    float e = dot_slide(dot, &band, controls);
    // the band covers the bottom third: subtitles go above it, about 35 px clear of it (64 % of a 16:9 picture on the
    // Brick; on the Smart Pro's taller picture, worked out from where it is), following it as it slides
    int above = 64;
    if (DY && video_dst.h > 0) { above = (BAND_Y - 35 - video_dst.y) * 100 / video_dst.h; above = above < 10 ? 10 : above > 100 ? 100 : above; }
    player_sub_above(100 - (int)lroundf((100 - above) * e));
    band_top = H;
    if (e > 0) draw_overlay(e);
    draw_skip(e);
  }
  if (screen == SCR_PLAY) dot_set_accent(dot, player_accent());   // over the film: its colour, as the controls
  if (screen == SCR_PLAY || picker_shown) draw_picker();
  draw_busy();   // waiting: over any screen (starting from the list, changing the sound from Options)
  if (notice_at && !dot_notice(dot, notice_icon, notice_label, notice_value, notice_at_n, notice_n, notice_at)) notice_at = 0;
  if (now < toast_until) {
    Uint32 left = toast_until - now;
    float a = left < DOT_FADE_MS ? left / (float)DOT_FADE_MS : 1;
    if (screen == SCR_PLAY && band_top < H) {   // over a film with the band up: above the band, not on it
      dot_set_alpha(dot, a); dot_pill(dot, W / 2, band_top - 24 - DOT_PILL_H, toast, 1, 0, DOT_CENTER); dot_set_alpha(dot, 1);
    } else dot_toast(dot, toast, a);
  }
}

// --- input ----------------------------------------------------------------------------------------------------
static void open_options(void) {
  opt_back = screen;
  opt_resume_play = screen == SCR_PLAY && !player_paused();
  if (opt_resume_play) player_pause(1);
  screen = SCR_OPTIONS;
}

static void close_options(void) {
  screen = opt_back;
  if (screen == SCR_PLAY && player_active()) {
    if (opt_resume_play) player_pause(0);
    overlay_until = SDL_GetTicks() + DOT_AUTOHIDE_MS;
  } else if (screen == SCR_PLAY) screen = SCR_BROWSE;
}

static int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }

static void change_option(int dir) {
  switch (opt_sel) {
    case O_ASPECT: settings.aspect = (settings.aspect + NASPECT + dir) % NASPECT; break;
    case O_BRIGHT: settings.brightness = clampi(settings.brightness + 5 * dir, -50, 50); break;
    case O_CONTRAST: settings.contrast = clampi(settings.contrast + 5 * dir, -50, 50); break;
    case O_DEINT: settings.deint = (settings.deint + NDEINT + dir) % NDEINT; break;
    case O_FILTER: settings.filter = (settings.filter + NFILTER + dir) % NFILTER; break;
    case O_SUBS:   // OFF < S < M < L < XL; A (dir 0) switches on/off and keeps the size
      if (!dir) settings.subs = !settings.subs;
      else if (!settings.subs) { if (dir > 0) { settings.subs = 1; settings.subsize = SUBSIZE_S; } }
      else if (settings.subsize + dir < 0) settings.subs = 0;
      else settings.subsize = clampi(settings.subsize + dir, 0, NSUBSIZE - 1);
      break;
    case O_TRACK: if (player_active()) player_cycle_sub(dir); return;
    case O_ATRACK: audio_switch(dir ? dir : 1); return;
    case O_AUTONEXT: settings.autonext = !settings.autonext; break;
    case O_PREFER: case O_PREFER2: case O_APREFER: case O_APREFER2: {   // every language Kino knows, and "" (the film's own / none) first
      int sub = opt_sel == O_PREFER || opt_sel == O_PREFER2, first = opt_sel == O_PREFER || opt_sel == O_APREFER;
      char *p1 = sub ? settings.prefer1 : settings.aprefer1, *p2 = sub ? settings.prefer2 : settings.aprefer2;
      char *cur = first ? p1 : p2, *other = first ? p2 : p1;
      int n = lang_count(), i = lang_index(cur), d = dir ? dir : 1;
      do { i += d; if (i >= n) i = -1; if (i < -1) i = n - 1; } while (i >= 0 && !strcmp(lang_code_at(i), other));
      snprintf(cur, 4, "%s", i < 0 ? "" : lang_code_at(i));
      if (!*p1 && *p2) { snprintf(p1, 4, "%s", p2); p2[0] = 0; }
      break;
    }
    case O_HWDEC: settings.hwdec = !settings.hwdec; break;
    case O_QUALITY: settings.quality = (settings.quality + NQUALITY + dir) % NQUALITY; break;   // from the next film
  }
  settings_save();
  if (player_active()) player_apply_settings();
}

// Quitting while a Jellyfin film plays (MENU, or killed by the system): tell the server where it stopped and to stop
// converting, before Kino is gone. Without it the server went on converting for nobody and the place was the last
// progress report. Skipped without Wi-Fi (each request would wait 8 s for nothing).
// Kino quitting while a Jellyfin film plays: the stop report goes out only after the player is gone (pos taken
// before), since a player still streaming asks for the next piece and the server starts a new converter for it.
static double jf_pos_at_quit = -1;   // MENU: where the film was when it closed
static void quit_jellyfin(double pos) {
  if (!jf_playing) return;
  if (wifi_state() != WIFI_OFF) jf_report_now(2, playing_jf.id, pos);
  jf_playing = 0;
  wifi_power_save(1);
}

static void on_button(int b) {
  if (b == BTN_MENU) {
    if (jf_playing) jf_pos_at_quit = player_pos();
    if (player_active()) { save_position(); player_close(); }   // Jellyfin's stop report: at the end of main
    running = 0; return;
  }
  if (screen == SCR_CONNECT) {
    if (b == BTN_B) screen = SCR_BROWSE;
    else if (b == BTN_A && !jf_code[0] && !job_th) jf_enter();
    return;
  }
  if (screen == SCR_NEXT) {   // the card goes out first (the main loop then plays it, or not)
    if ((b == BTN_A || b == BTN_B) && !next_leave) { next_leave = SDL_GetTicks(); next_go = b == BTN_A; }
    return;
  }
  if (screen == SCR_HELP) {
    if (b == BTN_L1 || b == BTN_R1 || b == BTN_LEFT || b == BTN_RIGHT) { help_tab = !help_tab; return; }
    if (b == BTN_A || b == BTN_B || b == BTN_X || b == BTN_Y) {
      screen = help_back;
      if (!settings.intro) { settings.intro = 1; settings_save(); }
    }
    return;
  }
  if (screen == SCR_OPTIONS) {
    switch (b) {
      case BTN_X: help_back = SCR_OPTIONS; help_tab = opt_back == SCR_PLAY; screen = SCR_HELP; break;
      case BTN_UP: case BTN_DOWN: {   // within the tab, round
        int t = opt_tab(opt_sel), a = TAB_FIRST[t], n = TAB_FIRST[t + 1] - a;
        opt_sel = a + (opt_sel - a + n + (b == BTN_UP ? -1 : 1)) % n; break;
      }
      case BTN_L1: case BTN_R1: opt_sel = TAB_FIRST[(opt_tab(opt_sel) + NTABS + (b == BTN_L1 ? -1 : 1)) % NTABS]; break;
      case BTN_LEFT: change_option(-1); break;
      case BTN_RIGHT: change_option(1); break;
      case BTN_A: change_option(opt_sel == O_SUBS ? 0 : 1); break;
      case BTN_B: case BTN_Y: close_options(); break;
    }
    return;
  }
  if (screen == SCR_PLAY && picker_on) { picker_button(b); return; }
  if (screen == SCR_PLAY && b == BTN_UP && !on_bar) { picker_open(); return; }
  if (screen == SCR_PLAY && b == BTN_X) {   // skip the intro, recap or credits Jellyfin marked
    const char *label; double to; int credits;
    if (skip_now(&label, &to, &credits)) {
      if (credits && !strcmp(playing_jf.type, "Episode") && settings.autonext) stop(1);   // to Up next
      else { player_seek_to(to); overlay_until = 0; }
    }
    return;
  }
  if (screen == SCR_PLAY && on_bar) {   // scrubbing: move the knob, A jumps there
    double len = player_duration();
    switch (b) {
      case BTN_LEFT: case BTN_RIGHT: case BTN_L1: case BTN_R1: {
        double step = len > 0 ? len / 50 : 10;
        if (b == BTN_L1 || b == BTN_R1) step *= 5;
        scrub_to += (b == BTN_LEFT || b == BTN_L1 ? -step : step);
        if (scrub_to < 0) scrub_to = 0;
        if (len > 0 && scrub_to > len - 1) scrub_to = len - 1;
        break;
      }
      case BTN_A: player_seek_to(scrub_to); on_bar = 0; break;
      case BTN_B: case BTN_UP: on_bar = 0; break;
    }
    overlay_until = SDL_GetTicks() + DOT_AUTOHIDE_MS;
    return;
  }
  if (screen == SCR_PLAY) {
    int was_visible = player_paused() || SDL_GetTicks() < overlay_until;
    overlay_until = SDL_GetTicks() + DOT_AUTOHIDE_MS;
    switch (b) {
      case BTN_DOWN: if (was_visible) { on_bar = 1; scrub_to = player_pos(); } break;
      case BTN_A: player_pause(!player_paused()); break;
      case BTN_LEFT: case BTN_RIGHT:
        seek_dir = b == BTN_LEFT ? -1 : 1;
        seek_until = SDL_GetTicks() + 450;
        player_seek(seek_dir * 10);
        break;
      case BTN_L1: case BTN_R1: case BTN_L2: case BTN_R2: {   // filter (L1/R1) and aspect (L2/R2), shown in a pill
        int dir = b == BTN_L1 || b == BTN_L2 ? -1 : 1;
        if (b == BTN_L1 || b == BTN_R1) {
          settings.filter = (settings.filter + NFILTER + dir) % NFILTER;
          show_notice(DOT_I_FILM, "FILTER", FILTER_NAMES[settings.filter], settings.filter + 1, NFILTER);
        } else {
          settings.aspect = (settings.aspect + NASPECT + dir) % NASPECT;
          show_notice(DOT_I_FILM, "ASPECT", ASPECT_NAMES[settings.aspect], settings.aspect + 1, NASPECT);
        }
        overlay_until = 0;
        settings_save();
        player_apply_settings();
        break;
      }
      case BTN_SELECT:
        settings.subs = !settings.subs;
        show_notice(DOT_I_FILM, "SUBTITLES", settings.subs ? "On" : "Off", 0, 0);
        overlay_until = 0;
        settings_save();
        player_apply_settings();
        break;
      case BTN_Y: open_options(); break;
      case BTN_B: stop(0); break;
    }
    return;
  }
  if (b == BTN_SELECT && grid_mode() && !(jf_mode && jf_loading)) { if (jf_mode) jf_sort_next(); else local_sort_next(); return; }
  if (in_grid()) {   // the grid: d-pad in 2D, L1/R1 a page of 10
    switch (b) {
      case BTN_LEFT: if (cursor) set_cursor(cursor - 1); return;
      case BTN_RIGHT: if (cursor + 1 < nitems) set_cursor(cursor + 1); return;
      case BTN_UP: if (cursor >= GCOLS) set_cursor(cursor - GCOLS); return;
      case BTN_DOWN: if (cursor + GCOLS < nitems) set_cursor(cursor + GCOLS); else if (cursor / GCOLS < (nitems - 1) / GCOLS) set_cursor(nitems - 1); return;
      case BTN_L1: set_cursor(cursor - GPAGE); return;
      case BTN_R1: set_cursor(cursor + GPAGE); return;
    }
  }
  switch (b) {
    case BTN_UP: if (nitems) set_cursor(cursor ? cursor - 1 : nitems - 1); break;
    case BTN_DOWN: if (nitems) set_cursor(cursor + 1 < nitems ? cursor + 1 : 0); break;
    case BTN_L1: case BTN_LEFT: set_cursor(cursor - ROWS); break;
    case BTN_R1: case BTN_RIGHT: set_cursor(cursor + ROWS); break;
    case BTN_A:
      if (jf_mode) {
        if (!nitems || jf_loading) break;
        if (jitems[cursor].folder) {
          jstack[jf_depth].cursor = cursor;
          jf_depth++; jstack[jf_depth].cursor = 0;
          jf_open(jitems[cursor].id, jitems[cursor].type, jitems[cursor].name);
        } else play(cursor, 0);
      } else if (nitems) { if (items[cursor].dir) enter_dir(); else play(cursor, 0); }
      break;
    case BTN_X: if (nitems && !items[cursor].dir && !(jf_mode && jf_loading)) play(cursor, 1); break;
    case BTN_B:
      if (jf_mode) { if (!jf_loading) jf_leave_level(); }
      else if (depth) leave_dir(); else running = 0;
      break;
    case BTN_Y: open_options(); break;
  }
}

// --- shots ----------------------------------------------------------------------------------------------------
static void pump(Uint32 ms) {   // runs the event loop without input for a while (shots)
  Uint32 end = SDL_GetTicks() + ms;
  while (SDL_GetTicks() < end) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) if (player_handle(&e)) stop(1);
    player_frame();
    SDL_Delay(5);
  }
}

static void shot(const char *dir, const char *name) {
  draw();
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
  SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
  char p[1200]; snprintf(p, sizeof p, "%s/%s.bmp", dir, name);
  SDL_SaveBMP(s, p); SDL_FreeSurface(s);
  printf("%s\n", p);
}

static void wait_media(void) {   // until the selected file's thumbnail and the visible facts are in (max 20 s)
  for (int t = 0; t < 400; t++) {
    char p[1400]; const uint8_t *rgb; Uint32 acc; MediaInfo mi; int ok = 1;
    if (cursor < nitems && !items[cursor].dir) { item_path(cursor, p, sizeof p); ok = media_thumb(p, &rgb, &acc); }
    for (int i = top; ok && i < nitems && i < top + ROWS; i++)
      if (!items[i].dir) { item_path(i, p, sizeof p); ok = media_info(p, &mi); }
    if (ok) return;
    SDL_Delay(50);
  }
}

static void shots(const char *out) {
  list_dir(); wait_media(); shot(out, "browse-root");
  for (int i = 0; i < nitems; i++) if (!strcmp(items[i].name, "Films")) { set_cursor(i); enter_dir(); break; }
  if (local_grid) {   // films with covers made from their frames
    for (int t = 0; t < 600; t++) {
      int all = 1; const uint8_t *rgb;
      for (int i = top; i < nitems && i < top + GPAGE; i++) all &= !*cover_of(i) || media_poster(cover_of(i), &rgb);
      MediaInfo mi; char p[1400]; item_path(cursor, p, sizeof p);
      if (all && (items[cursor].dir || media_info(p, &mi))) break;
      SDL_Delay(50);
    }
    shot(out, "local-grid");
  }
  leave_dir();
  // Into the series, with episode 3 started.
  for (int i = 0; i < nitems; i++) if (!strcmp(items[i].name, "Series")) { set_cursor(i); enter_dir(); break; }
  if (nitems && items[0].dir) enter_dir();
  for (int i = 0; i < nitems; i++) {
    char p[1400]; MediaInfo mi; item_path(i, p, sizeof p);
    for (int t = 0; t < 200 && !media_info(p, &mi); t++) { media_want(NULL, (const char *[]){ p }, 1); SDL_Delay(50); }
    if (!media_info(p, &mi)) { fprintf(stderr, "shots: no facts for %s\n", p); mi.duration = 30; }
    if (i < 2) store_set_resume(p, -1, 0);
    if (i == 2) store_set_resume(p, mi.duration * .42, mi.duration);
  }
  set_cursor(2); wait_media(); shot(out, "browse-series");
  settings.ground = 1; shot(out, "browse-series-ground-centre"); settings.ground = 0;
  set_cursor(3); wait_media(); shot(out, "browse-new");
  if (nitems > ROWS) { set_cursor(nitems - 2); wait_media(); shot(out, "browse-scrolled"); set_cursor(3); }
  set_cursor(3); wait_media();   // Up next: episode 3 ended, episode 4 starts in 7 s
  next_kind = 1; next_local = 3; item_path(3, next_pic, sizeof next_pic); next_at = SDL_GetTicks() + 6500; screen = SCR_NEXT;
  shot(out, "next-episode");
  next_kind = 0; screen = SCR_BROWSE;
  play(2, 0); pump(1500); overlay_until = 0; pump(4500);   // the crop holds for 4 s before it's taken
  shot(out, "playing-subtitles");   // playing, no controls: the .srt next to it (build/shots.sh) in Noto Sans
  { int was = settings.aspect;   // Stretch and 4:3 change the picture's shape, never the subtitles'
    settings.aspect = ASPECT_STRETCH; player_apply_settings(); pump(800); shot(out, "playing-subtitles-stretch");
    settings.aspect = ASPECT_4_3; player_apply_settings(); pump(800); shot(out, "playing-subtitles-4-3");
    settings.aspect = was; player_apply_settings(); pump(400); }
  player_seek(-8); pump(600);   // back a little: the 10 s seek below must stay inside the 30 s test episode
  player_pause(1); draw(); pump(400);   // drawn once first: the subtitles go above the band before the shot
  shot(out, "playing");
  on_button(BTN_RIGHT); pump(200); shot(out, "playing-seek");
  on_button(BTN_DOWN); for (int i = 0; i < 12; i++) on_button(BTN_RIGHT);
  shot(out, "playing-scrub"); on_button(BTN_B);
  for (int i = 0; i < FILTER_CRT; i++) on_button(BTN_R1);
  pump(600); player_frame(); shot(out, "playing-crt");
  on_button(BTN_R1); on_button(BTN_R2); pump(300); shot(out, "playing-aspect"); notice_at = 0;
  on_button(BTN_UP); shot(out, "playing-picker"); on_button(BTN_DOWN); pump(300); shot(out, "playing-picker-sub"); on_button(BTN_B); settings.subs = 1; player_apply_settings();
  overlay_until = 0; player_pause(0); pump(800); shot(out, "playing-clean");
  int q = settings.quality;   // the two Wi-Fi warnings: stalls at 4 Mbit/s, then drops at 2
  settings.quality = QUALITY_4; net_poor = 0; net_stalls = 2; net_watch(); shot(out, "playing-wifi-slow");
  settings.quality = QUALITY_2; net_poor = 0; lost_tries = 1; net_watch(); shot(out, "playing-wifi-drops");
  settings.quality = q; net_poor = net_stalls = lost_tries = 0; toast_until = 0;
  open_options(); opt_sel = O_BRIGHT; pump(300); shot(out, "options-night");
  close_options(); stop(0); shot(out, "browse-toast");
  toast_until = 0; open_options(); opt_sel = O_ASPECT; shot(out, "options");
  opt_sel = O_SUBS; settings.subsize = SUBSIZE_L; shot(out, "options-subsize"); settings.subsize = SUBSIZE_M;
  opt_sel = O_HWDEC; shot(out, "options-brick");
  opt_sel = O_APREFER; snprintf(settings.aprefer1, 4, "nor"); snprintf(settings.aprefer2, 4, "eng"); shot(out, "options-audio");
  settings.aprefer1[0] = settings.aprefer2[0] = 0;
  on_button(BTN_X); shot(out, "help-from-options"); on_button(BTN_B);
  help_back = SCR_BROWSE; help_tab = 0; screen = SCR_HELP; shot(out, "help-first-start");
  help_tab = 1; shot(out, "help-playing"); help_tab = 0; screen = SCR_OPTIONS;
  screen = SCR_CONNECT; snprintf(jf_code, sizeof jf_code, "722223");
  snprintf(connect_msg, sizeof connect_msg, "http://192.168.1.20:8096"); shot(out, "jellyfin-connect");
  if (!jf_logged_in()) return;
  // With a login in the user folder: the real libraries, the first grid, and a series opened.
  screen = SCR_BROWSE; jf_enter();
  for (int t = 0; t < 400 && (job_th || jf_loading); t++) { SDL_Delay(25); if (job_th && SDL_AtomicGet(&job_done)) job_finished(); }
  wait_media(); shot(out, "jf-libraries");
  #define JF_WAIT() for (int t = 0; t < 400 && (job_th || jf_loading); t++) { SDL_Delay(25); if (job_th && SDL_AtomicGet(&job_done)) job_finished(); }
  for (int sp = 0; sp < 2 && sp < nitems && !strcmp(jitems[sp].type, "Special"); sp++) {   // Continue watching, Next up
    set_cursor(sp); on_button(BTN_A); JF_WAIT();
    wait_media(); for (int t = 0; t < 100 && strcmp(thumb_for, "") == 0; t++) SDL_Delay(30);
    shot(out, sp ? "jf-nextup" : "jf-resume");
    jf_leave_level(); JF_WAIT();
  }
  int first_lib = 0; while (first_lib < nitems && !strcmp(jitems[first_lib].type, "Special")) first_lib++;
  for (int lib = first_lib, nlib = nitems; lib < nlib && lib < first_lib + 2; lib++) {
    if (lib > first_lib) { jf_leave_level(); for (int t = 0; t < 400 && (job_th || jf_loading); t++) { SDL_Delay(25); if (job_th && SDL_AtomicGet(&job_done)) job_finished(); } }
    set_cursor(lib); on_button(BTN_A);
    for (int t = 0; t < 400 && (job_th || jf_loading); t++) { SDL_Delay(25); if (job_th && SDL_AtomicGet(&job_done)) job_finished(); }
    if (jf_grid) {
      set_cursor(1);
      for (int t = 0; t < 300; t++) {   // until the covers in view are in
        int all = 1; const uint8_t *rgb;
        for (int i = top; i < nitems && i < top + GPAGE + GCOLS && i < nitems; i++) { char u[600]; jf_poster_url(&jitems[i], POSTER_W, POSTER_H, u, sizeof u); all &= media_poster(u, &rgb); }
        if (all) break;
        SDL_Delay(50);
      }
      char nm[32]; snprintf(nm, sizeof nm, "jf-grid-%d", lib); shot(out, nm);
      if (lib == first_lib && nitems > GPAGE * 2) {   // scrolled down: the row before peeks out under the header
        set_cursor(GPAGE + 1);
        for (int t = 0; t < 300; t++) {
          int all = 1; const uint8_t *rgb;
          for (int i = top - GCOLS; i < nitems && i < top + GPAGE + GCOLS; i++) { char u[600]; jf_poster_url(&jitems[i], POSTER_W, POSTER_H, u, sizeof u); all &= media_poster(u, &rgb); }
          if (all) break;
          SDL_Delay(50);
        }
        shot(out, "jf-grid-scrolled"); set_cursor(1);
      }
      if (lib == first_lib) {   // the other two sorts (back to the user's sort after)
        int was = settings.sort;
        for (int k = 1; k < NSORT; k++) {
          jf_sort_next(); want_media();
          for (int t = 0; t < 300; t++) {
            int all = 1; const uint8_t *rgb;
            for (int i = top; i < nitems && i < top + GPAGE + GCOLS && i < nitems; i++) { char u[600]; jf_poster_url(&jitems[i], POSTER_W, POSTER_H, u, sizeof u); all &= media_poster(u, &rgb); }
            if (all) break;
            SDL_Delay(50);
          }
          snprintf(nm, sizeof nm, "jf-grid-sort-%d", settings.sort); shot(out, nm);
        }
        settings.sort = was; settings_save(); jf_view(NULL); toast_until = 0; notice_at = 0;
      }
    }
  }
}

// --- main -----------------------------------------------------------------------------------------------------
static double cpu_seconds(void) {
  struct rusage u; getrusage(RUSAGE_SELF, &u);
  return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

static int bench(const char *file, double secs) {
  const char *flt = getenv("KINO_FILTER");   // measure a filter: KINO_FILTER=2 kino --bench ...
  if (flt) { settings.filter = atoi(flt) % NFILTER; player_apply_settings(); }
  char url[1400];
  if (!strncmp(file, "jf:", 3)) {   // jf:<item id>: as the app plays it (KINO_USER = folder with jellyfin.txt)
    const char *u = getenv("KINO_USER");
    jf_load(u ? u : ".");
    JfItem it = { 0 }; snprintf(it.id, sizeof it.id, "%s", file + 3);
    int how = jf_playback(&it, url, sizeof url, QUALITY_BPS[settings.quality]);
    fprintf(stderr, "jellyfin: %s\n", how == 2 ? "transcoded" : how == 1 ? "direct" : "failed");
    file = url;
  }
  player_open(file, 0);
  long frames = 0; Uint32 t0 = 0; double c0 = 0;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) if (player_handle(&e)) running = 0;
    if (player_frame()) {
      if (!t0) { t0 = SDL_GetTicks(); c0 = cpu_seconds(); }
      draw_video(); SDL_RenderPresent(ren); frames++;
    } else SDL_Delay(2);
    if (t0 && SDL_GetTicks() - t0 >= secs * 1000) break;
  }
  if (t0) player_report(file, (SDL_GetTicks() - t0) / 1000.0, frames, cpu_seconds() - c0);
  return 0;
}

int main(int argc, char **argv) {
  int is_bench = argc >= 4 && !strcmp(argv[1], "--bench");
  const char *user = !is_bench && argc > 1 ? argv[1] : "/tmp/kino";
  const char *pak = !is_bench && argc > 2 ? argv[2] : ".";
  const char *shots_dir = argc > 5 && !strcmp(argv[3], "--shots") ? argv[4] : NULL;
  mkdir(user, 0755);
  if (shots_dir) { snprintf(root, sizeof root, "%s", argv[5]); snprintf(home, sizeof home, "%s", root); }
  else {
    const char *sd = getenv("SDCARD_PATH"); if (!sd || !*sd) sd = "/mnt/SDCARD";
    snprintf(root, sizeof root, "%s", sd);
    snprintf(home, sizeof home, "%s/Videos", sd);
    mkdir(home, 0755);
  }
  snprintf(cwd, sizeof cwd, "%s", home);
  depth = strcmp(home, root) ? 1 : 0;   // in Videos, one level under the card (B goes up to it)

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
  dot_screen_detect(); grid_fit();   // Brick 1024x768 or Smart Pro 1280x720
  if (W != 1024) TAB_NAMES[3] = "Device";
  SDL_Window *win = SDL_CreateWindow("Kino", 0, 0, W, H, shots_dir ? 0 : SDL_WINDOW_FULLSCREEN);
  ren = SDL_CreateRenderer(win, -1, shots_dir ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
  if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
  SDL_RenderSetLogicalSize(ren, W, H);
  for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
  char mpvdir[1200]; snprintf(mpvdir, sizeof mpvdir, "%s/mpv", pak);
  if (player_init(ren, !shots_dir, mpvdir) != 0) { fprintf(stderr, "mpv failed to start\n"); return 1; }
  if (is_bench) { int r = bench(argv[2], atof(argv[3])); player_quit(); SDL_Quit(); return r; }

  char p[1200]; snprintf(p, sizeof p, "%s/fonts", pak);
  if (!(dot = dot_init(ren, p, DOT_ACCENT_KINO))) { fprintf(stderr, "fonts in %s: %s\n", p, SDL_GetError()); return 1; }
  { static const DotTheme kino = DOT_THEME_KINO; dot_set_theme(dot, &kino); }   // midnight blue
  { time_t now = time(NULL); char d[32]; strftime(d, sizeof d, "%Y-%m-%d %H:%M", localtime(&now));
    fprintf(stderr, "Kino %s, %s\n", KINO_VERSION, d); }
  store_open(user);
  jf_load(user);
  if (!shots_dir) wifi_start();
  snprintf(p, sizeof p, "%s/cache", user);
  media_start(p);

  shots_mode = shots_dir != NULL;
  dot_set_still(dot, shots_mode);   // screenshots: everything drawn where it ends up (Dot, Motion)
  if (shots_dir) shots(shots_dir);
  else {
    list_dir();
    if (!settings.intro) { help_back = SCR_BROWSE; screen = SCR_HELP; }
    // KINO_PLAY=<Jellyfin item id>[@seconds]: straight into that film (test/net.sh); KINO_SHOTS=<dir> as above
    const char *ap = getenv("KINO_PLAY"); net_shots = getenv("KINO_SHOTS");
    if (ap && *ap && jf_logged_in()) {
      signal(SIGUSR1, on_usr1); net_test = 1;
      JfItem it = { 0 }; snprintf(it.id, sizeof it.id, "%.*s", (int)strcspn(ap, "@"), ap);
      snprintf(it.name, sizeof it.name, "%s", it.id); snprintf(it.type, sizeof it.type, "Movie");
      if (strchr(ap, '@')) it.pos = atof(strchr(ap, '@') + 1);
      screen = SCR_BROWSE; play_jf(&it, 0);
    }
    signal(SIGTERM, on_term);
    { void *w[2]; backtrace(w, 2); }   // loads libgcc now: not safe inside the signal handler
    signal(SIGUSR2, on_usr2); main_thread = pthread_self();
    SDL_Thread *wd = SDL_CreateThread(watchdog, "watchdog", NULL); if (wd) SDL_DetachThread(wd);
    int held = -1; Uint32 repeat_at = 0;
    while (running) {
      loop_tick = SDL_GetTicks();
      SDL_Event e;
      // menus: about 60 frames a second, so lists and cards move smoothly (Dot's motion); playing: the film paces it
      static Uint32 frame_at;   // when the last frame was drawn: wait only for what's left of this one
      int period = screen == SCR_PLAY ? 33 : 16, since = (int)(SDL_GetTicks() - frame_at);
      int r, got = SDL_WaitEventTimeout(&e, since >= period ? 1 : period - since);
      while (got) {
        if (e.type == SDL_QUIT) running = 0;
        else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
          int b = e.cbutton.button;
          on_button(b);
          if (b == BTN_UP || b == BTN_DOWN || b == BTN_LEFT || b == BTN_RIGHT) { held = b; repeat_at = SDL_GetTicks() + 400; }
        } else if (e.type == SDL_CONTROLLERBUTTONUP && e.cbutton.button == held) held = -1;
        else if (e.type == SDL_CONTROLLERAXISMOTION && (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                                                        e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
          static int down[2];   // a trigger counts as a press when it crosses half way
          int i = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT, now_down = e.caxis.value > 16000;
          if (now_down && !down[i]) on_button(i ? BTN_R2 : BTN_L2);
          down[i] = now_down;
        }
        else if ((r = player_handle(&e)) && screen != SCR_BROWSE) {
          if (screen == SCR_OPTIONS) screen = SCR_PLAY;
          if (r == 1) stop(1); else stream_lost("broke off");
        }
        got = SDL_PollEvent(&e);
      }
      if (held >= 0 && SDL_GetTicks() >= repeat_at) { on_button(held); repeat_at = SDL_GetTicks() + 70; }
      if (screen == SCR_PLAY && SDL_GetTicks() - last_save > 30000) { save_position(); last_save = SDL_GetTicks(); }
      if (jf_playing && player_active() && SDL_GetTicks() - last_report > 10000) {
        jf_report(1, playing_jf.id, player_pos(), player_paused()); last_report = SDL_GetTicks();
        static int reports;   // in the log: every 10 s in tests, every minute otherwise (for logs users send in)
        if (net_test || ++reports % 6 == 0) {
          char w[120]; wifi_describe(w, sizeof w); net_log(NULL, "Wi-Fi: %s", w);
          net_log(NULL, "at %.0f s%s, %.0f s ahead, %ld kB/s in", player_pos(), player_buffering() >= 0 ? ", waiting" : "",
                  player_cache_secs(), player_net_speed() / 1000);
        }
      }
      if (job_th && SDL_AtomicGet(&job_done)) job_finished();
      if (retry_at && SDL_GetTicks() >= retry_at && !job_th && jf_playing) {   // ask the server again: a fresh URL
        retry_at = 0; playing_jf.pos = player_pos(); jf_play_from_start = 0; start_job(J_PLAY);
      }
      if (term_asked) running = 0;
      if (net_reset) {
        net_reset = 0; net_stalls = net_poor = 0; net_drops0 = wifi_drops();
        net_log(NULL, "-- next test: warnings reset");
      }
      if (jf_playing && player_active()) net_watch();
      // Stuck: waiting with nothing arriving (FFmpeg can go on reconnecting quietly, or the server sends nothing): 40 s
      // in the middle of a film, 75 s while it opens (the server starts a converter for a stream it converts, which
      // took 36 s in tests). Then it counts as broken off: the tries above ask the server again, and give up in the end.
      // Data coming in (the read-ahead reaching further: mpv's own speed figure keeps its last value through an outage)
      // means it's slow, not stuck, and starts the count again.
      static Uint32 stuck_since; static double cache_end;
      int waiting = player_buffering();
      double end = player_cache_end(), grew = end > cache_end + 0.5;
      if (end >= 0) cache_end = end;
      if (jf_playing && player_active() && !player_paused() && !retry_at && !job_th && waiting >= 0 && !grew) {
        Uint32 limit = waiting == 101 ? 75000 : 40000;
        if (!stuck_since) stuck_since = SDL_GetTicks();
        else if (SDL_GetTicks() - stuck_since > limit) { stuck_since = 0; stream_lost("stuck waiting"); }
      } else stuck_since = 0;
      if (lost_tries && !retry_at && player_active() && player_buffering() < 0 && player_pos() > playing_jf.pos + 10)
        { lost_tries = 0; net_log(NULL, "reconnected, playing for 10 s at %.0f s", player_pos()); }   // a later drop: five new tries
      if (screen == SCR_CONNECT && jf_code[0] && !job_th && SDL_GetTicks() >= next_poll) start_job(J_POLL);
      if (screen == SCR_NEXT && SDL_GetTicks() >= next_at && !job_th && !next_leave) { next_leave = SDL_GetTicks(); next_go = 1; }
      if (screen == SCR_NEXT && next_leave && SDL_GetTicks() - next_leave >= DOT_OUT_MS && !job_th) {
        next_leave = 0;
        if (next_go) start_next(); else { next_kind = 0; screen = SCR_BROWSE; }
      }
      int fresh = player_active() && player_frame();
      if (screen == SCR_PLAY && !fresh && !player_paused() && !on_bar && SDL_GetTicks() >= overlay_until + DOT_SLIDE_MS + 50 &&
          !notice_at && !busy_shown && !picker_shown && !lost_tries && player_buffering() < 0) continue;
      frame_at = SDL_GetTicks();
      draw();
      SDL_RenderPresent(ren);
    }
  }
  loop_tick = 0;   // quitting: blocking on the way out is fine
  double quit_pos = jf_pos_at_quit >= 0 ? jf_pos_at_quit : player_pos();
  if (player_active()) { save_position(); player_close(); }
  store_save();
  media_stop();
  if (thumb_tex) SDL_DestroyTexture(thumb_tex);
  for (int i = 0; i < 32; i++) if (ptex[i].t) SDL_DestroyTexture(ptex[i].t);
  if (crt_tex) SDL_DestroyTexture(crt_tex);
  dot_quit(dot);
  player_quit();   // mpv gone: nothing streams any more
  quit_jellyfin(quit_pos);
  SDL_Quit();
  return 0;
}
