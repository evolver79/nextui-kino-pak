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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>

enum { BTN_A = SDL_CONTROLLER_BUTTON_B, BTN_B = SDL_CONTROLLER_BUTTON_A, BTN_X = SDL_CONTROLLER_BUTTON_Y,
       BTN_Y = SDL_CONTROLLER_BUTTON_X, BTN_START = SDL_CONTROLLER_BUTTON_START, BTN_SELECT = SDL_CONTROLLER_BUTTON_BACK,
       BTN_MENU = SDL_CONTROLLER_BUTTON_GUIDE, BTN_L1 = SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BTN_R1 = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
       BTN_UP = SDL_CONTROLLER_BUTTON_DPAD_UP, BTN_DOWN = SDL_CONTROLLER_BUTTON_DPAD_DOWN,
       BTN_LEFT = SDL_CONTROLLER_BUTTON_DPAD_LEFT, BTN_RIGHT = SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
       BTN_L2 = SDL_CONTROLLER_BUTTON_MAX + 1, BTN_R2 = SDL_CONTROLLER_BUTTON_MAX + 2 };   // triggers are axes in SDL
enum { SCR_BROWSE, SCR_PLAY, SCR_OPTIONS, SCR_CONNECT };
enum { O_ASPECT, O_BRIGHT, O_CONTRAST, O_DEINT, O_FILTER, O_SUBS, O_TRACK, O_PREFER, O_LEDS, O_HWDEC, O_GROUND, NOPT };
#define ROWS 5
#define MAXDEPTH 32

typedef struct { char name[256]; int dir, virt; } Item;   // virt: the Jellyfin row at the SD card root

static SDL_Renderer *ren;
static Dot *dot;
static int running = 1, screen = SCR_BROWSE, opt_back, opt_sel, opt_resume_play;
static char root[1024], cwd[1100];
static Item *items;
static int nitems, cursor, top, depth, stack[MAXDEPTH];
static char playing[1400], playing_title[256];
static Uint32 overlay_until, toast_until, seek_until;
static int seek_dir;   // -1/1: the rewind/forward button shows the last seek for a moment
static int on_bar;     // overlay focus on the progress bar (scrubbing); scrub_to = where A would jump
static double scrub_to;
static char info[48];   // "ASPECT · 16:9" pill after L2/R2 or L1/R1 while playing
static Uint32 info_until;
static SDL_Texture *crt_tex;
static char toast[64];
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
static Uint32 last_report;
// Background jobs (network): one at a time.
enum { J_NONE, J_CONNECT, J_POLL, J_LIST };
static SDL_Thread *job_th;
static SDL_atomic_t job_done;
static int job_kind, job_result, job_n;
static JfItem *job_items;
static char job_id[40], job_type[32], jf_code[16], jf_server[256], connect_msg[160];
static Uint32 next_poll;
static int shots_mode;
// Poster grid (Jellyfin films and series): 5 x 2 covers; textures for the covers in view, by url.
#define GCOLS 5
#define GPAGE 10
static int jf_grid;
typedef struct { char url[600]; SDL_Texture *t; Uint32 used; } PosterTex;
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

static void item_path(int i, char *out, size_t n) { snprintf(out, n, "%s/%s", cwd, items[i].name); }

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

// --- browsing -------------------------------------------------------------------------------------------------
static void want_media(void) {
  if (!(jf_mode && jf_grid)) media_want_posters(NULL, 0);
  if (jf_mode && jf_grid) {   // the covers in view, then the next page
    static char buf[GPAGE * 2][600]; const char *urls[GPAGE * 2]; int n = 0;
    for (int i = top; i < nitems && i < top + GPAGE * 2; i++) {
      jf_poster_url(&jitems[i], POSTER_W, POSTER_H, buf[n], sizeof buf[n]); urls[n] = buf[n]; n++;
    }
    media_want(NULL, NULL, 0);
    media_want_posters(urls, n);
    return;
  }
  if (jf_mode) {   // the detail card's picture comes from the server (FFmpeg decodes the JPEG)
    char url[600] = "";
    if (cursor < nitems) jf_image_url(&jitems[cursor], THUMB_W, THUMB_H, url, sizeof url);
    media_want(url, NULL, 0);
    return;
  }
  const char *paths[ROWS]; static char buf[ROWS][1400];
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
  if (jf_mode && jf_grid) {   // the grid scrolls a row of 5 at a time
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
    items[nitems++].dir = dir;
  }
  if (d) closedir(d);
  if (nitems) qsort(items, nitems, sizeof *items, item_cmp);
  if (!depth && !shots_mode) {   // the SD card root starts with Jellyfin
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
static int job_run(void *u) {
  (void)u;
  switch (job_kind) {
    case J_CONNECT:
      if (!jf_discover(jf_server, sizeof jf_server)) job_result = -2;
      else job_result = jf_qc_start(jf_server, jf_code, sizeof jf_code) ? 1 : -1;
      break;
    case J_POLL: job_result = jf_qc_poll(); break;
    case J_LIST: job_n = job_id[0] ? jf_children(job_id, job_type, &job_items) : jf_views(&job_items); break;
  }
  SDL_AtomicSet(&job_done, 1);
  return 0;
}
static void start_job(int kind) {
  if (job_th) return;
  job_kind = kind; SDL_AtomicSet(&job_done, 0);
  job_th = SDL_CreateThread(job_run, "job", NULL);
}

static void jf_show(JfItem *list, int n) {   // the listed items become the browser's rows
  free(jitems); jitems = list;
  free(items); items = calloc(n > 0 ? n : 1, sizeof *items); nitems = n > 0 ? n : 0;
  for (int i = 0; i < nitems; i++) {
    JfItem *it = &jitems[i];
    if (!strcmp(it->type, "Episode") && it->index)
      snprintf(items[i].name, sizeof items[i].name, "%d · %s", it->index, it->name);
    else snprintf(items[i].name, sizeof items[i].name, "%s", it->name);
    items[i].dir = it->folder;
  }
  // Films, series and collections as a poster grid; everything else (seasons, episodes, home videos) as rows.
  jf_grid = nitems > 0;
  for (int i = 0; i < nitems; i++)
    if (strcmp(jitems[i].type, "Movie") && strcmp(jitems[i].type, "Series") && strcmp(jitems[i].type, "BoxSet")) jf_grid = 0;
  top = 0;
  set_cursor(0);
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

static void jf_enter(void) {   // from the SD card root
  if (!jf_logged_in()) {
    screen = SCR_CONNECT; jf_code[0] = 0;
    snprintf(connect_msg, sizeof connect_msg, "Looking for Jellyfin on the network…");
    start_job(J_CONNECT);
    return;
  }
  jf_mode = 1; jf_depth = 0; jstack[0].cursor = 0;
  free(items); items = NULL; nitems = 0;
  jf_open("", "", "Jellyfin");
}

static void jf_leave_level(void) {
  if (jf_depth == 0) {   // back to the SD card
    jf_mode = 0; jf_grid = 0; free(jitems); jitems = NULL;
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
  char *s = strrchr(cwd, '/'); if (s) *s = 0;
  list_dir();
  set_cursor(stack[--depth]);
}

static void select_path(const char *path) {   // put the cursor on this file if it's in the list
  for (int i = 0; i < nitems; i++) {
    char p[1400]; item_path(i, p, sizeof p);
    if (!strcmp(p, path)) { set_cursor(i); return; }
  }
}

// --- playing --------------------------------------------------------------------------------------------------
static void show_toast(const char *s) { snprintf(toast, sizeof toast, "%s", s); toast_until = SDL_GetTicks() + 2600; }

static void play(int i, int from_start) {
  media_want(NULL, NULL, 0);    // nothing in the background while a film plays: covers and pictures
  media_want_posters(NULL, 0);  // compete for the Wi-Fi and the CPU (they come back in the browser)
  if (jf_mode) {
    playing_jf = jitems[i]; jf_playing = 1;
    jf_stream_url(&playing_jf, playing, sizeof playing);
    if (!strcmp(playing_jf.type, "Episode") && playing_jf.index)
      snprintf(playing_title, sizeof playing_title, "S%d · E%d · %s", playing_jf.season, playing_jf.index, playing_jf.name);
    else snprintf(playing_title, sizeof playing_title, "%s", playing_jf.name);
    player_open(playing, from_start ? 0 : playing_jf.pos);
    jf_report(0, playing_jf.id, from_start ? 0 : playing_jf.pos, 0);
    screen = SCR_PLAY; on_bar = 0;
    overlay_until = SDL_GetTicks() + DOT_AUTOHIDE_MS;
    last_save = last_report = SDL_GetTicks();
    return;
  }
  jf_playing = 0;
  item_path(i, playing, sizeof playing);
  title_of(items[i].name, playing_title, sizeof playing_title);
  double pos = 0;
  if (!from_start && store_resume(playing, &pos, NULL) != RES_PARTIAL) pos = 0;
  player_open(playing, pos);
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
  on_bar = 0;
  if (jf_playing) {
    double pos = ended ? playing_jf.runtime : player_pos();
    jf_report(2, playing_jf.id, pos, 0);
    if (!ended) player_close();
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
  if (ended && cursor + 1 < nitems && !items[cursor + 1].dir) set_cursor(cursor + 1);   // A plays the next one
  want_media();
}

// --- drawing --------------------------------------------------------------------------------------------------
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
    SDL_Rect d = { x, y, w, h };
    SDL_RenderCopy(ren, thumb_tex, NULL, &d);
    dot_corners(dot, x, y, w, h, 20, DOT_CARD);
  } else {
    dot_rect(dot, x, y, w, h, 20, DOT_RAISED, 255);
    dot_icon(dot, is_dir ? DOT_I_FOLDER : DOT_I_FILM, x + w / 2, y + h / 2, 72, DOT_DIM);
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
  else if (!strcmp(it->type, "Season")) snprintf(out, n, it->unplayed ? "Season · %d new" : "Season", it->unplayed);
  else if (!strncmp(it->type, "CollectionFolder", 16) || !strcmp(it->type, "UserView")) snprintf(out, n, "Library");
  else snprintf(out, n, "%s", it->folder ? "Folder" : len);
}

static void draw_detail_jf(void) {
  dot_card(dot, 40, 96, 460, 528);
  if (jf_loading || !nitems) {
    draw_thumb_area(56, 112, "", 1);
    dot_text(dot, DOT_TITLE, jf_loading ? "Loading…" : "Nothing here", 70, 374, DOT_TEXT, DOT_LEFT, 400);
    return;
  }
  JfItem *it = &jitems[cursor];
  char url[600], buf[64];
  jf_image_url(it, THUMB_W, THUMB_H, url, sizeof url);
  draw_thumb_area(56, 112, url, 0);
  dot_text(dot, DOT_TITLE, it->name, 70, 374, DOT_TEXT, DOT_LEFT, 400);
  jf_label(it, buf, sizeof buf);
  dot_text(dot, DOT_LABEL_T, buf, 70, 424, DOT_LABEL, DOT_LEFT, 400);
  if (it->folder) return;
  Uint32 acc = !strcmp(thumb_for, url) ? thumb_accent : DOT_ACCENT_KINO;
  if (it->pos > 0 && !it->played) {
    dot_adot(dot, 70, 488, acc);
    dot_text(dot, DOT_LABEL_T, "Resume", 102, 484, DOT_TEXT, DOT_LEFT, 0);
    fmt_time(it->pos, buf, sizeof buf);
  } else {
    dot_text(dot, DOT_LABEL_T, it->played ? "Seen" : "Length", 70, 484, DOT_LABEL, DOT_LEFT, 0);
    if (it->runtime > 0) fmt_time(it->runtime, buf, sizeof buf); else snprintf(buf, sizeof buf, "--:--");
  }
  dot_time(dot, DOT_DISPLAY_L, buf, 68, 512, DOT_TEXT, DOT_LEFT);
  dot_progress(dot, 70, 590, 400, it->played ? 1 : it->runtime > 0 ? (float)(it->pos / it->runtime) : 0, 0);
}

static void draw_detail(void) {
  if (jf_mode) { draw_detail_jf(); return; }
  dot_card(dot, 40, 96, 460, 528);
  if (!nitems) {
    draw_thumb_area(56, 112, "", 1);
    dot_text(dot, DOT_TITLE, "No videos here", 70, 374, DOT_TEXT, DOT_LEFT, 400);
    dot_text(dot, DOT_BODY_S, "Put films and series in the Videos folder on the SD card.", 70, 424, DOT_LABEL, DOT_LEFT, 400);
    return;
  }
  Item *it = &items[cursor];
  char path[1400], title[256], buf[64];
  item_path(cursor, path, sizeof path);
  title_of(it->name, title, sizeof title);
  draw_thumb_area(56, 112, path, it->dir);
  dot_text(dot, DOT_TITLE, it->dir ? it->name : title, 70, 374, DOT_TEXT, DOT_LEFT, 400);
  if (it->virt) {
    dot_text(dot, DOT_LABEL_T, jf_logged_in() ? "Server · logged in" : "Server · not connected", 70, 424, DOT_LABEL, DOT_LEFT, 400);
    if (jf_logged_in()) dot_text(dot, DOT_BODY_S, jf_user_name(), 70, 470, DOT_TEXT, DOT_LEFT, 400);
    else dot_text(dot, DOT_BODY_S, "Press A to connect with Quick Connect.", 70, 470, DOT_LABEL, DOT_LEFT, 400);
    return;
  }
  if (it->dir) {
    snprintf(buf, sizeof buf, folder_count == 1 ? "Folder · 1 item" : "Folder · %d items", folder_count);
    dot_text(dot, DOT_LABEL_T, buf, 70, 424, DOT_LABEL, DOT_LEFT, 400);
    return;
  }
  MediaInfo mi; int known = media_info(path, &mi);
  if (known) { media_label(&mi, buf, sizeof buf); dot_text(dot, DOT_LABEL_T, buf, 70, 424, DOT_LABEL, DOT_LEFT, 400); }
  double pos = 0, dur = known ? mi.duration : 0;
  int st = store_resume(path, &pos, NULL);
  Uint32 acc = !strcmp(thumb_for, path) ? thumb_accent : DOT_ACCENT_KINO;
  if (st == RES_PARTIAL) {
    dot_adot(dot, 70, 488, acc);
    dot_text(dot, DOT_LABEL_T, "Resume", 102, 484, DOT_TEXT, DOT_LEFT, 0);
    fmt_time(pos, buf, sizeof buf);
  } else {
    dot_text(dot, DOT_LABEL_T, st == RES_SEEN ? "Seen" : "Length", 70, 484, DOT_LABEL, DOT_LEFT, 0);
    if (dur > 0) fmt_time(dur, buf, sizeof buf); else snprintf(buf, sizeof buf, "--:--");
  }
  dot_time(dot, DOT_DISPLAY_L, buf, 68, 512, DOT_TEXT, DOT_LEFT);
  dot_progress(dot, 70, 590, 400, st == RES_SEEN ? 1 : st == RES_PARTIAL && dur > 0 ? (float)(pos / dur) : 0, 0);
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

static void draw_grid(void) {   // 5 x 2 covers; the focused one's name in the header
  JfItem *f = &jitems[cursor];
  char label[96], lab[64];
  jf_label(f, lab, sizeof lab);
  snprintf(label, sizeof label, "%s%s%d / %d", lab, *lab ? " · " : "", cursor + 1, nitems);
  int lw = dot_text_w(dot, DOT_LABEL_T, label);
  dot_text(dot, DOT_TITLE, f->name, 40, 22, 0x161616, DOT_LEFT, 944 - lw - 40);
  dot_text(dot, DOT_LABEL_T, label, 984, 30, 0x161616, DOT_RIGHT, 0);
  for (int i = top; i < nitems && i < top + GPAGE; i++) {
    JfItem *it = &jitems[i];
    char url[600]; jf_poster_url(it, POSTER_W, POSTER_H, url, sizeof url);
    int x = 40 + (i - top) % GCOLS * (POSTER_W + 16), y = 96 + (i - top) / GCOLS * (POSTER_H + 16);
    float prog = !it->folder && !it->played && it->pos > 0 && it->runtime > 0 ? (float)(it->pos / it->runtime) : 0;
    dot_poster(dot, poster_tex(url), x, y, POSTER_W, POSTER_H, i == cursor, prog, it->played, it->folder && it->unplayed > 0);
  }
  DotHint h[4]; int n = 0;
  if (f->folder) h[n++] = (DotHint){ "A", "Open" };
  else h[n++] = (DotHint){ "A", f->pos > 0 && !f->played ? "Resume" : "Play" };
  h[n++] = (DotHint){ "L1 R1", "Page" };
  h[n++] = (DotHint){ "B", "Back" };
  h[n++] = (DotHint){ "Y", "Options" };
  dot_dock(dot, h, n);
}

static void draw_browse(void) {
  dot_set_ground(dot, settings.ground ? DOT_GROUND_TOPCENTRE : DOT_GROUND_TOPLEFT);
  dot_ground(dot);
  if (jf_mode && jf_grid && !jf_loading && nitems) { draw_grid(); return; }
  // Crumb: "SD card / Videos / ...", only the last two folders when it's deeper than that.
  char crumb[700] = "SD card / Videos", count[32], rel[1100];
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
  snprintf(count, sizeof count, nitems == 1 ? "1 item" : "%d items", nitems);
  dot_header(dot, crumb, count);
  draw_detail();
  for (int i = top; i < nitems && i < top + ROWS; i++) {
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
    dot_row(dot, 524, 96 + 94 * (i - top), 460, items[i].virt ? DOT_I_TV : items[i].dir ? DOT_I_FOLDER : DOT_I_FILM,
            items[i].dir && !jf_mode ? items[i].name : title, NULL, meta, i == cursor ? DOT_FOCUS : 0, 0);
  }
  DotHint h[4]; int n = 0;
  if (jf_mode) {
    if (nitems && !jf_loading) {
      JfItem *it = &jitems[cursor];
      if (it->folder) h[n++] = (DotHint){ "A", "Open" };
      else if (it->pos > 0 && !it->played) { h[n++] = (DotHint){ "A", "Resume" }; h[n++] = (DotHint){ "X", "From start" }; }
      else h[n++] = (DotHint){ "A", "Play" };
    }
    h[n++] = (DotHint){ "B", "Back" };
    h[n++] = (DotHint){ "Y", "Options" };
    dot_dock(dot, h, n);
    return;
  }
  if (nitems && items[cursor].dir) h[n++] = (DotHint){ "A", "Open" };
  else if (nitems) {
    char path[1400]; item_path(cursor, path, sizeof path);
    if (store_resume(path, NULL, NULL) == RES_PARTIAL) { h[n++] = (DotHint){ "A", "Resume" }; h[n++] = (DotHint){ "X", "From start" }; }
    else h[n++] = (DotHint){ "A", "Play" };
  }
  h[n++] = (DotHint){ "B", depth ? "Back" : "Quit" };
  h[n++] = (DotHint){ "Y", "Options" };
  dot_dock(dot, h, n);
}

static void draw_video(void) {
  SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
  SDL_RenderClear(ren);
  SDL_Rect d;
  SDL_Texture *t = player_texture(&d);
  if (t) SDL_RenderCopy(ren, t, NULL, &d);
  if (t && settings.filter == FILTER_CRT) {   // scanlines every third row and a soft vignette, made once
    if (!crt_tex) {
      static Uint32 px[W * H];
      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
          float dx = (x - W / 2.f) / (W / 2.f), dy = (y - H / 2.f) / (H / 2.f), v = (dx * dx + dy * dy) * .5f;
          int a = (int)(v * v * 255 * .55f) + (y % 3 == 2 ? 110 : 0);
          px[y * W + x] = (Uint32)(a > 255 ? 255 : a) << 24;
        }
      crt_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, W, H);
      SDL_UpdateTexture(crt_tex, NULL, px, W * 4);
      SDL_SetTextureBlendMode(crt_tex, SDL_BLENDMODE_BLEND);
    }
    SDL_Rect c = d;
    if (SDL_IntersectRect(&d, &(SDL_Rect){ 0, 0, W, H }, &c)) SDL_RenderCopy(ren, crt_tex, &c, &c);
  }
}

static void draw_overlay(void) {
  dot_set_accent(dot, player_accent());
  char buf[64], dur[32];
  dot_pill(dot, 40, 36, playing_title, 1, 0, DOT_LEFT);
  MediaInfo mi;
  if (media_info(playing, &mi)) { media_audio_label(&mi, buf, sizeof buf); dot_pill(dot, 984, 36, buf, 0, 1, DOT_RIGHT); }
  double pos = player_pos(), len = player_duration();
  int paused = player_paused();
  if (on_bar) pos = scrub_to;
  dot_card(dot, 40, 500, 944, 232);
  fmt_time(pos, buf, sizeof buf);
  int tw = dot_time(dot, DOT_DISPLAY_XL, buf, 76, 526, DOT_TEXT, DOT_LEFT);
  int lx = 76 + tw + 30;
  fmt_time(len, dur, sizeof dur); snprintf(buf, sizeof buf, "/ %s", dur);
  dot_text(dot, DOT_LABEL_T, buf, lx, 546, DOT_LABEL, DOT_LEFT, 0);
  dot_adot(dot, lx, 588, dot_accent(dot));
  dot_text(dot, DOT_LABEL_T, paused ? "Paused" : "Playing", lx + 32, 584, DOT_TEXT, DOT_LEFT, 0);
  int seeking = !on_bar && SDL_GetTicks() < seek_until ? seek_dir : 0;   // the pressed side is lit, with the ring
  dot_round(dot, 702, 570, DOT_I_BACK, 0, seeking < 0 ? DOT_FOCUS | DOT_ON : 0);
  dot_round(dot, 806, 570, paused ? DOT_I_PLAY : DOT_I_PAUSE, 1, seeking || on_bar ? 0 : DOT_FOCUS);
  dot_round(dot, 900, 570, DOT_I_FWD, 0, seeking > 0 ? DOT_FOCUS | DOT_ON : 0);
  dot_progress(dot, 76, 644, 872, len > 0 ? (float)(pos / len) : 0, on_bar ? 2 : 1);
  dot_text(dot, DOT_LABEL_T, on_bar ? "◀ ▶ move · L1 R1 faster · A jump here · B cancel" : "◀ ▶ 10 s · ▼ scrub · L1 R1 filter · L2 R2 aspect",
           76, 690, DOT_LABEL, DOT_LEFT, 872);
}

static const char *DEINTS[NDEINT] = { "AUTO", "ON", "OFF" };

static void draw_options(void) {
  int night = opt_back == SCR_PLAY;
  if (night) { draw_video(); dot_scrim(dot); }
  else { dot_set_ground(dot, settings.ground ? DOT_GROUND_TOPCENTRE : DOT_GROUND_TOPLEFT); dot_ground(dot); }
  dot_set_night(dot, night);
  dot_title(dot, "OPTIONS");
  dot_header(dot, NULL, "B close");
  int f[NOPT]; for (int i = 0; i < NOPT; i++) f[i] = opt_sel == i ? DOT_FOCUS : 0;
  // Two columns of 464, cards 16 apart; heights follow their content (chips wrap to two lines).
  int P = DOT_OPT_PITCH, ha = dot_panel_h(6 + 2 * DOT_CHIP_H + 10), hp = dot_panel_h(4 * P - 8), hs = dot_panel_h(3 * P - 8), hb = dot_panel_h(3 * P - 8);
  int y = dot_panel(dot, 40, 96, 464, ha, "Aspect");
  dot_chips(dot, 70, y + 6, 404, ASPECT_NAMES, NASPECT, settings.aspect, opt_sel == O_ASPECT ? settings.aspect : -1);
  y = dot_panel(dot, 40, 96 + ha + 16, 464, hp, "Picture");
  dot_opt_slider(dot, 70, y, 404, "Brightness", (settings.brightness + 50) / 100.f, f[O_BRIGHT]);
  dot_opt_slider(dot, 70, y + P, 404, "Contrast", (settings.contrast + 50) / 100.f, f[O_CONTRAST]);
  dot_opt(dot, 70, y + 2 * P, 404, "Deinterlace", DEINTS[settings.deint], f[O_DEINT]);
  char fname[32]; snprintf(fname, sizeof fname, "%s", FILTER_NAMES[settings.filter]);
  for (char *q = fname; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
  dot_opt(dot, 70, y + 3 * P, 404, "Filter", fname, f[O_FILTER]);
  y = dot_panel(dot, 520, 96, 464, hs, "Subtitles");
  dot_opt_toggle(dot, 550, y, 404, "Show", settings.subs, f[O_SUBS]);
  dot_opt(dot, 550, y + P, 404, "Track", player_sub_label(), f[O_TRACK] | (player_active() ? 0 : DOT_OFF));
  dot_opt(dot, 550, y + 2 * P, 404, "Prefer", PREFER_LABELS[settings.prefer], f[O_PREFER]);
  y = dot_panel(dot, 520, 96 + hs + 16, 464, hb, "Brick");
  dot_opt_toggle(dot, 550, y, 404, "Lights follow video", settings.leds, f[O_LEDS]);
  dot_opt_toggle(dot, 550, y + P, 404, "Hardware decoding", settings.hwdec, f[O_HWDEC]);
  dot_opt(dot, 550, y + 2 * P, 404, "Ground light", settings.ground ? "CENTRE" : "LEFT", f[O_GROUND]);
  DotHint h[] = { { "A", "Change" }, { "<>", "Adjust" }, { "B", "Close" } };
  dot_dock(dot, h, 3);
  dot_set_night(dot, 0);
}

static void draw_connect(void) {   // Quick Connect: the code in dots, approved from the Jellyfin app
  dot_set_ground(dot, settings.ground ? DOT_GROUND_TOPCENTRE : DOT_GROUND_TOPLEFT);
  dot_ground(dot);
  dot_title(dot, "JELLYFIN");
  dot_header(dot, NULL, "B back");
  int y = dot_panel(dot, 40, 96, 944, 440, "Quick Connect");
  if (jf_code[0]) {
    char spaced[16]; snprintf(spaced, sizeof spaced, "%.3s %.3s", jf_code, jf_code + 3);
    dot_text(dot, DOT_DISPLAY_XL, spaced, 70, y + 20, DOT_TEXT, DOT_LEFT, 0);
    dot_text(dot, DOT_BODY_S, "In Jellyfin on your phone or computer, open your profile, choose Quick Connect", 70, y + 150, DOT_TEXT, DOT_LEFT, 884);
    dot_text(dot, DOT_BODY_S, "and enter this code. Kino logs in by itself.", 70, y + 184, DOT_TEXT, DOT_LEFT, 884);
    dot_adot(dot, 70, y + 262, DOT_ACCENT_KINO);
    dot_text(dot, DOT_LABEL_T, "Waiting", 102, y + 258, DOT_TEXT, DOT_LEFT, 0);
    dot_text(dot, DOT_LABEL_T, connect_msg, 70, y + 300, DOT_LABEL, DOT_LEFT, 884);
  } else {
    dot_text(dot, DOT_TITLE, connect_msg, 70, y + 30, DOT_TEXT, DOT_LEFT, 884);
  }
  DotHint h[2]; int n = 0;
  if (!jf_code[0] && !job_th) h[n++] = (DotHint){ "A", "Try again" };
  h[n++] = (DotHint){ "B", "Back" };
  dot_dock(dot, h, n);
}

static void draw(void) {
  dot_frame(dot);
  dot_set_accent(dot, DOT_ACCENT_KINO);
  Uint32 now = SDL_GetTicks();
  if (screen == SCR_BROWSE) draw_browse();
  else if (screen == SCR_CONNECT) draw_connect();
  else if (screen == SCR_OPTIONS) draw_options();
  else {
    draw_video();
    if (player_paused() || on_bar || now < overlay_until) draw_overlay();
    if (now < info_until) dot_pill(dot, W / 2, 36, info, 0, 1, DOT_CENTER);
  }
  if (now < toast_until) {
    Uint32 left = toast_until - now;
    dot_toast(dot, toast, left < DOT_FADE_MS ? left / (float)DOT_FADE_MS : 1);
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
    case O_SUBS: settings.subs = !settings.subs; break;
    case O_TRACK: if (player_active()) player_cycle_sub(dir); return;
    case O_PREFER: settings.prefer = (settings.prefer + NPREFER + dir) % NPREFER; break;
    case O_LEDS: settings.leds = !settings.leds; break;
    case O_HWDEC: settings.hwdec = !settings.hwdec; break;
    case O_GROUND: settings.ground = !settings.ground; break;
  }
  settings_save();
  if (player_active()) player_apply_settings();
}

static void on_button(int b) {
  if (b == BTN_MENU) {
    if (player_active()) { save_position(); player_close(); }
    running = 0; return;
  }
  if (screen == SCR_CONNECT) {
    if (b == BTN_B) screen = SCR_BROWSE;
    else if (b == BTN_A && !jf_code[0] && !job_th) jf_enter();
    return;
  }
  if (screen == SCR_OPTIONS) {
    switch (b) {
      case BTN_UP: opt_sel = (opt_sel + NOPT - 1) % NOPT; break;
      case BTN_DOWN: opt_sel = (opt_sel + 1) % NOPT; break;
      case BTN_LEFT: change_option(-1); break;
      case BTN_RIGHT: case BTN_A: change_option(1); break;
      case BTN_B: case BTN_Y: close_options(); break;
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
          snprintf(info, sizeof info, "Filter · %s", FILTER_NAMES[settings.filter]);
        } else {
          settings.aspect = (settings.aspect + NASPECT + dir) % NASPECT;
          snprintf(info, sizeof info, "Aspect · %s", ASPECT_NAMES[settings.aspect]);
        }
        info_until = SDL_GetTicks() + 1500;
        overlay_until = 0;
        settings_save();
        player_apply_settings();
        break;
      }
      case BTN_Y: open_options(); break;
      case BTN_B: stop(0); break;
    }
    return;
  }
  if (jf_mode && jf_grid && !jf_loading && nitems) {   // the grid: d-pad in 2D, L1/R1 a page of 10
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
  play(2, 0); pump(1500); player_pause(1); pump(400);
  shot(out, "playing");
  on_button(BTN_RIGHT); pump(200); shot(out, "playing-seek");
  on_button(BTN_DOWN); for (int i = 0; i < 12; i++) on_button(BTN_RIGHT);
  shot(out, "playing-scrub"); on_button(BTN_B);
  for (int i = 0; i < FILTER_CRT; i++) on_button(BTN_R1);
  pump(600); player_frame(); shot(out, "playing-crt");
  on_button(BTN_R1); on_button(BTN_R2); pump(300); shot(out, "playing-aspect");
  overlay_until = 0; player_pause(0); pump(800); shot(out, "playing-clean");
  open_options(); opt_sel = O_BRIGHT; pump(300); shot(out, "options-night");
  close_options(); stop(0); shot(out, "browse-toast");
  toast_until = 0; open_options(); opt_sel = O_ASPECT; shot(out, "options");
  screen = SCR_CONNECT; snprintf(jf_code, sizeof jf_code, "722223");
  snprintf(connect_msg, sizeof connect_msg, "http://192.168.68.152:8096"); shot(out, "jellyfin-connect");
  if (!jf_logged_in()) return;
  // With a login in the user folder: the real libraries, the first grid, and a series opened.
  screen = SCR_BROWSE; jf_enter();
  for (int t = 0; t < 400 && (job_th || jf_loading); t++) { SDL_Delay(25); if (job_th && SDL_AtomicGet(&job_done)) job_finished(); }
  wait_media(); shot(out, "jf-libraries");
  for (int lib = 0, nlib = nitems; lib < nlib && lib < 4; lib++) {
    if (lib) { jf_leave_level(); for (int t = 0; t < 400 && (job_th || jf_loading); t++) { SDL_Delay(25); if (job_th && SDL_AtomicGet(&job_done)) job_finished(); } }
    set_cursor(lib); on_button(BTN_A);
    for (int t = 0; t < 400 && (job_th || jf_loading); t++) { SDL_Delay(25); if (job_th && SDL_AtomicGet(&job_done)) job_finished(); }
    if (jf_grid) {
      set_cursor(1);
      for (int t = 0; t < 300; t++) {   // until the covers in view are in
        int all = 1; const uint8_t *rgb;
        for (int i = top; i < nitems && i < top + GPAGE; i++) { char u[600]; jf_poster_url(&jitems[i], POSTER_W, POSTER_H, u, sizeof u); all &= media_poster(u, &rgb); }
        if (all) break;
        SDL_Delay(50);
      }
      char nm[32]; snprintf(nm, sizeof nm, "jf-grid-%d", lib); shot(out, nm);
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
  if (shots_dir) snprintf(root, sizeof root, "%s", argv[5]);
  else {
    const char *sd = getenv("SDCARD_PATH"); if (!sd || !*sd) sd = "/mnt/SDCARD";
    snprintf(root, sizeof root, "%s/Videos", sd);
    mkdir(root, 0755);
  }
  snprintf(cwd, sizeof cwd, "%s", root);

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
  SDL_Window *win = SDL_CreateWindow("Kino", 0, 0, W, H, shots_dir ? 0 : SDL_WINDOW_FULLSCREEN);
  ren = SDL_CreateRenderer(win, -1, shots_dir ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
  if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
  SDL_RenderSetLogicalSize(ren, W, H);
  for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
  if (player_init(ren, !shots_dir) != 0) { fprintf(stderr, "mpv failed to start\n"); return 1; }
  if (is_bench) { int r = bench(argv[2], atof(argv[3])); player_quit(); SDL_Quit(); return r; }

  char p[1200]; snprintf(p, sizeof p, "%s/fonts", pak);
  if (!(dot = dot_init(ren, p, DOT_ACCENT_KINO))) { fprintf(stderr, "fonts in %s: %s\n", p, SDL_GetError()); return 1; }
  store_open(user);
  jf_load(user);
  if (!shots_dir) leds_start();
  snprintf(p, sizeof p, "%s/cache", user);
  media_start(p);

  shots_mode = shots_dir != NULL;
  if (shots_dir) shots(shots_dir);
  else {
    list_dir();
    int held = -1; Uint32 repeat_at = 0;
    while (running) {
      SDL_Event e;
      int got = SDL_WaitEventTimeout(&e, 33);
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
        else if (player_handle(&e) && screen != SCR_BROWSE) { if (screen == SCR_OPTIONS) screen = SCR_PLAY; stop(1); }
        got = SDL_PollEvent(&e);
      }
      if (held >= 0 && SDL_GetTicks() >= repeat_at) { on_button(held); repeat_at = SDL_GetTicks() + 70; }
      if (screen == SCR_PLAY && SDL_GetTicks() - last_save > 30000) { save_position(); last_save = SDL_GetTicks(); }
      if (jf_playing && player_active() && SDL_GetTicks() - last_report > 10000) {
        jf_report(1, playing_jf.id, player_pos(), player_paused()); last_report = SDL_GetTicks();
      }
      if (job_th && SDL_AtomicGet(&job_done)) job_finished();
      if (screen == SCR_CONNECT && jf_code[0] && !job_th && SDL_GetTicks() >= next_poll) start_job(J_POLL);
      int fresh = player_active() && player_frame();
      if (player_active() && screen != SCR_BROWSE) leds_follow(player_accent()); else leds_restore();
      if (screen == SCR_PLAY && !fresh && !player_paused() && !on_bar && SDL_GetTicks() >= overlay_until + 100 &&
          SDL_GetTicks() >= info_until + 100) continue;
      draw();
      SDL_RenderPresent(ren);
    }
  }
  leds_restore();
  store_save();
  media_stop();
  if (thumb_tex) SDL_DestroyTexture(thumb_tex);
  for (int i = 0; i < 32; i++) if (ptex[i].t) SDL_DestroyTexture(ptex[i].t);
  if (crt_tex) SDL_DestroyTexture(crt_tex);
  dot_quit(dot);
  player_quit();
  SDL_Quit();
  return 0;
}
