// Kino: video player for NextUI on the TrimUI Brick, drawn with the shared Dot UI (ui/dot).
//   kino <user_dir> <pak_dir>                              the app: browse SD card/Videos, play, options, resume
//   kino <user_dir> <pak_dir> --shots <out_dir> <videos>   renders the screens to BMPs (headless, for review)
//   kino --bench <file> <seconds>                          plays a file, then prints frames, drops and CPU
#define _GNU_SOURCE
#include "kino.h"
#include "dot.h"
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
       BTN_LEFT = SDL_CONTROLLER_BUTTON_DPAD_LEFT, BTN_RIGHT = SDL_CONTROLLER_BUTTON_DPAD_RIGHT };
enum { SCR_BROWSE, SCR_PLAY, SCR_OPTIONS };
enum { O_ASPECT, O_BRIGHT, O_CONTRAST, O_DEINT, O_SUBS, O_TRACK, O_PREFER, O_LEDS, O_GROUND, NOPT };
#define ROWS 5
#define MAXDEPTH 32

typedef struct { char name[256]; int dir; } Item;

static SDL_Renderer *ren;
static Dot *dot;
static int running = 1, screen = SCR_BROWSE, opt_back, opt_sel, opt_resume_play;
static char root[1024], cwd[1100];
static Item *items;
static int nitems, cursor, top, depth, stack[MAXDEPTH];
static char playing[1400], playing_title[256];
static Uint32 overlay_until, toast_until, seek_until;
static int seek_dir;   // -1/1: the rewind/forward button shows the last seek for a moment
static char toast[64];
static SDL_Texture *thumb_tex;
static char thumb_for[1400];
static Uint32 thumb_accent;
static int folder_count = -1;
static Uint32 last_save;

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
  const char *paths[ROWS]; static char buf[ROWS][1400];
  int n = 0;
  for (int i = top; i < nitems && i < top + ROWS; i++)
    if (!items[i].dir) { item_path(i, buf[n], sizeof buf[n]); paths[n] = buf[n]; n++; }
  char sel[1400] = "";
  if (cursor < nitems && !items[cursor].dir) item_path(cursor, sel, sizeof sel);
  media_want(sel, paths, n);
  folder_count = -1;
  if (cursor < nitems && items[cursor].dir) { item_path(cursor, sel, sizeof sel); folder_count = count_dir(sel); }
}

static void set_cursor(int c) {
  cursor = c < 0 ? 0 : c >= nitems ? (nitems ? nitems - 1 : 0) : c;
  if (cursor < top) top = cursor;
  if (cursor >= top + ROWS) top = cursor - ROWS + 1;
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
  top = 0;
  set_cursor(0);
}

static void enter_dir(void) {
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
  item_path(i, playing, sizeof playing);
  title_of(items[i].name, playing_title, sizeof playing_title);
  double pos = 0;
  if (!from_start && store_resume(playing, &pos, NULL) != RES_PARTIAL) pos = 0;
  player_open(playing, pos);
  screen = SCR_PLAY;
  overlay_until = SDL_GetTicks() + DOT_AUTOHIDE_MS;
  last_save = SDL_GetTicks();
}

static void save_position(void) {
  double pos = player_pos(), dur = player_duration();
  if (dur > 0) { store_set_resume(playing, pos, dur); store_save(); }
}

static void stop(int ended) {
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

static void draw_detail(void) {
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

static void draw_browse(void) {
  dot_set_ground(dot, settings.ground ? DOT_GROUND_TOPCENTRE : DOT_GROUND_TOPLEFT);
  dot_ground(dot);
  // Crumb: "SD card / Videos / ...", only the last two folders when it's deeper than that.
  char crumb[700] = "SD card / Videos", count[32], rel[1100];
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
    if (!items[i].dir) {
      item_path(i, path, sizeof path);
      double pos, dur; MediaInfo mi;
      int st = store_resume(path, &pos, &dur);
      if (st == RES_SEEN) snprintf(meta, sizeof meta, "SEEN");
      else if (st == RES_PARTIAL && dur > 0) snprintf(meta, sizeof meta, "%d%%", (int)(100 * pos / dur));
      else if (media_info(path, &mi) && mi.duration > 0) fmt_time(mi.duration, meta, sizeof meta);
    }
    dot_row(dot, 524, 96 + 94 * (i - top), 460, items[i].dir ? DOT_I_FOLDER : DOT_I_FILM,
            items[i].dir ? items[i].name : title, NULL, meta, i == cursor ? DOT_FOCUS : 0, 0);
  }
  DotHint h[4]; int n = 0;
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
}

static void draw_overlay(void) {
  dot_set_accent(dot, player_accent());
  char buf[64], dur[32];
  dot_pill(dot, 40, 36, playing_title, 1, 0, DOT_LEFT);
  MediaInfo mi;
  if (media_info(playing, &mi)) { media_audio_label(&mi, buf, sizeof buf); dot_pill(dot, 984, 36, buf, 0, 1, DOT_RIGHT); }
  double pos = player_pos(), len = player_duration();
  int paused = player_paused();
  dot_card(dot, 40, 500, 944, 232);
  fmt_time(pos, buf, sizeof buf);
  int tw = dot_time(dot, DOT_DISPLAY_XL, buf, 76, 526, DOT_TEXT, DOT_LEFT);
  int lx = 76 + tw + 30;
  fmt_time(len, dur, sizeof dur); snprintf(buf, sizeof buf, "/ %s", dur);
  dot_text(dot, DOT_LABEL_T, buf, lx, 546, DOT_LABEL, DOT_LEFT, 0);
  dot_adot(dot, lx, 588, dot_accent(dot));
  dot_text(dot, DOT_LABEL_T, paused ? "Paused" : "Playing", lx + 32, 584, DOT_TEXT, DOT_LEFT, 0);
  int seeking = SDL_GetTicks() < seek_until ? seek_dir : 0;   // the pressed side is lit and has the focus ring
  dot_round(dot, 702, 570, DOT_I_BACK, 0, seeking < 0 ? DOT_FOCUS | DOT_ON : 0);
  dot_round(dot, 806, 570, paused ? DOT_I_PLAY : DOT_I_PAUSE, 1, seeking ? 0 : DOT_FOCUS);
  dot_round(dot, 900, 570, DOT_I_FWD, 0, seeking > 0 ? DOT_FOCUS | DOT_ON : 0);
  dot_progress(dot, 76, 644, 872, len > 0 ? (float)(pos / len) : 0, 1);
  dot_text(dot, DOT_LABEL_T, "◀ ▶ 10 s · L1 R1 1 min · Y options · B stop", 76, 690, DOT_LABEL, DOT_LEFT, 872);
}

static const char *ASPECTS[NASPECT] = { "Original", "4:3", "16:9", "Stretch", "Zoom" };
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
  int P = DOT_OPT_PITCH, ha = dot_panel_h(6 + 2 * DOT_CHIP_H + 10), hp = dot_panel_h(3 * P - 8), hs = hp, hb = dot_panel_h(2 * P - 8);
  int y = dot_panel(dot, 40, 96, 464, ha, "Aspect");
  dot_chips(dot, 70, y + 6, 404, ASPECTS, NASPECT, settings.aspect, opt_sel == O_ASPECT ? settings.aspect : -1);
  y = dot_panel(dot, 40, 96 + ha + 16, 464, hp, "Picture");
  dot_opt_slider(dot, 70, y, 404, "Brightness", (settings.brightness + 50) / 100.f, f[O_BRIGHT]);
  dot_opt_slider(dot, 70, y + P, 404, "Contrast", (settings.contrast + 50) / 100.f, f[O_CONTRAST]);
  dot_opt(dot, 70, y + 2 * P, 404, "Deinterlace", DEINTS[settings.deint], f[O_DEINT]);
  y = dot_panel(dot, 520, 96, 464, hs, "Subtitles");
  dot_opt_toggle(dot, 550, y, 404, "Show", settings.subs, f[O_SUBS]);
  dot_opt(dot, 550, y + P, 404, "Track", player_sub_label(), f[O_TRACK] | (player_active() ? 0 : DOT_OFF));
  dot_opt(dot, 550, y + 2 * P, 404, "Prefer", PREFER_LABELS[settings.prefer], f[O_PREFER]);
  y = dot_panel(dot, 520, 96 + hs + 16, 464, hb, "Brick");
  dot_opt_toggle(dot, 550, y, 404, "Lights follow video", settings.leds, f[O_LEDS]);
  dot_opt(dot, 550, y + P, 404, "Ground light", settings.ground ? "CENTRE" : "LEFT", f[O_GROUND]);
  DotHint h[] = { { "A", "Change" }, { "<>", "Adjust" }, { "B", "Close" } };
  dot_dock(dot, h, 3);
  dot_set_night(dot, 0);
}

static void draw(void) {
  dot_frame(dot);
  dot_set_accent(dot, DOT_ACCENT_KINO);
  Uint32 now = SDL_GetTicks();
  if (screen == SCR_BROWSE) draw_browse();
  else if (screen == SCR_OPTIONS) draw_options();
  else {
    draw_video();
    if (player_paused() || now < overlay_until) draw_overlay();
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
    case O_SUBS: settings.subs = !settings.subs; break;
    case O_TRACK: if (player_active()) player_cycle_sub(dir); return;
    case O_PREFER: settings.prefer = (settings.prefer + NPREFER + dir) % NPREFER; break;
    case O_LEDS: settings.leds = !settings.leds; break;
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
  if (screen == SCR_PLAY) {
    overlay_until = SDL_GetTicks() + DOT_AUTOHIDE_MS;
    switch (b) {
      case BTN_A: player_pause(!player_paused()); break;
      case BTN_LEFT: case BTN_RIGHT: case BTN_L1: case BTN_R1:
        seek_dir = b == BTN_LEFT || b == BTN_L1 ? -1 : 1;
        seek_until = SDL_GetTicks() + 450;
        player_seek(seek_dir * (b == BTN_L1 || b == BTN_R1 ? 60 : 10));
        break;
      case BTN_Y: open_options(); break;
      case BTN_B: stop(0); break;
    }
    return;
  }
  switch (b) {
    case BTN_UP: if (nitems) set_cursor(cursor ? cursor - 1 : nitems - 1); break;
    case BTN_DOWN: if (nitems) set_cursor(cursor + 1 < nitems ? cursor + 1 : 0); break;
    case BTN_L1: case BTN_LEFT: set_cursor(cursor - ROWS); break;
    case BTN_R1: case BTN_RIGHT: set_cursor(cursor + ROWS); break;
    case BTN_A: if (nitems) { if (items[cursor].dir) enter_dir(); else play(cursor, 0); } break;
    case BTN_X: if (nitems && !items[cursor].dir) play(cursor, 1); break;
    case BTN_B: if (depth) leave_dir(); else running = 0; break;
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
    while (!media_info(p, &mi)) { media_want(NULL, (const char *[]){ p }, 1); SDL_Delay(50); }
    if (i < 2) store_set_resume(p, -1, 0);
    if (i == 2) store_set_resume(p, mi.duration * .42, mi.duration);
  }
  set_cursor(2); wait_media(); shot(out, "browse-series");
  settings.ground = 1; shot(out, "browse-series-ground-centre"); settings.ground = 0;
  set_cursor(3); wait_media(); shot(out, "browse-new");
  play(2, 0); pump(1500); player_pause(1); pump(400);
  shot(out, "playing");
  on_button(BTN_RIGHT); pump(200); shot(out, "playing-seek");
  overlay_until = 0; player_pause(0); pump(800); shot(out, "playing-clean");
  open_options(); opt_sel = O_BRIGHT; pump(300); shot(out, "options-night");
  close_options(); stop(0); shot(out, "browse-toast");
  toast_until = 0; open_options(); opt_sel = O_ASPECT; shot(out, "options");
}

// --- main -----------------------------------------------------------------------------------------------------
static double cpu_seconds(void) {
  struct rusage u; getrusage(RUSAGE_SELF, &u);
  return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

static int bench(const char *file, double secs) {
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
  snprintf(p, sizeof p, "%s/cache", user);
  media_start(p);

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
        else if (player_handle(&e) && screen != SCR_BROWSE) { if (screen == SCR_OPTIONS) screen = SCR_PLAY; stop(1); }
        got = SDL_PollEvent(&e);
      }
      if (held >= 0 && SDL_GetTicks() >= repeat_at) { on_button(held); repeat_at = SDL_GetTicks() + 70; }
      if (screen == SCR_PLAY && SDL_GetTicks() - last_save > 30000) { save_position(); last_save = SDL_GetTicks(); }
      int fresh = player_active() && player_frame();
      if (screen == SCR_PLAY && !fresh && !player_paused() && SDL_GetTicks() >= overlay_until + 100) continue;
      draw();
      SDL_RenderPresent(ren);
    }
  }
  store_save();
  media_stop();
  if (thumb_tex) SDL_DestroyTexture(thumb_tex);
  dot_quit(dot);
  player_quit();
  SDL_Quit();
  return 0;
}
