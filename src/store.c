// Resume positions (<user>/resume.txt: "pos<TAB>duration<TAB>path", pos -1 = seen) and settings
// (<user>/settings.txt: "key=value"). Both are small text files written in full when they change.
#include "kino.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *path; double pos, dur; } Res;
static Res *res;
static int nres, capres, dirty;
static char user[1024];

Settings settings = { ASPECT_ORIGINAL, 0, 0, DEINT_AUTO, 1, 0, 0, 0, 1, FILTER_NONE, SORT_NAME, 0 };
const char *SORT_NAMES[NSORT] = { "Name", "Recently added", "Unwatched" };
const char *FILTER_NAMES[NFILTER] = { "None", "Sharpen", "Warm", "Cool", "Black & white", "Retro CRT" };
const char *ASPECT_NAMES[NASPECT] = { "Original", "4:3", "16:9", "Stretch", "Zoom" };
const char *PREFER_LANGS[NPREFER] = { "eng,en", "nor,nob,no,nb,en,eng", "nor,nob,no,nb", "", "eng,en,nor,nob,no,nb" };
const char *PREFER_LABELS[NPREFER] = { "ENG", "NOR, ENG", "NOR", "FILE", "ENG, NOR" };

static Res *find(const char *path) {
  for (int i = 0; i < nres; i++) if (!strcmp(res[i].path, path)) return &res[i];
  return NULL;
}

static void settings_load(void) {
  char p[1100]; snprintf(p, sizeof p, "%s/settings.txt", user);
  FILE *f = fopen(p, "r"); if (!f) return;
  char k[64]; int v;
  while (fscanf(f, " %63[^=]=%d", k, &v) == 2) {
    if (!strcmp(k, "aspect") && v >= 0 && v < NASPECT) settings.aspect = v;
    else if (!strcmp(k, "brightness")) settings.brightness = v < -50 ? -50 : v > 50 ? 50 : v;
    else if (!strcmp(k, "contrast")) settings.contrast = v < -50 ? -50 : v > 50 ? 50 : v;
    else if (!strcmp(k, "deinterlace") && v >= 0 && v < NDEINT) settings.deint = v;
    else if (!strcmp(k, "subtitles")) settings.subs = !!v;
    else if (!strcmp(k, "prefer") && v >= 0 && v < NPREFER) settings.prefer = v;
    else if (!strcmp(k, "leds")) settings.leds = !!v;
    else if (!strcmp(k, "ground")) settings.ground = !!v;
    else if (!strcmp(k, "hwdec")) settings.hwdec = !!v;
    else if (!strcmp(k, "filter") && v >= 0 && v < NFILTER) settings.filter = v;
    else if (!strcmp(k, "sort") && v >= 0 && v < NSORT) settings.sort = v;
    else if (!strcmp(k, "intro")) settings.intro = !!v;
  }
  fclose(f);
}

void settings_save(void) {
  char p[1100]; snprintf(p, sizeof p, "%s/settings.txt", user);
  FILE *f = fopen(p, "w"); if (!f) return;
  fprintf(f, "aspect=%d\nbrightness=%d\ncontrast=%d\ndeinterlace=%d\nsubtitles=%d\nprefer=%d\nleds=%d\nground=%d\nhwdec=%d\nfilter=%d\nsort=%d\nintro=%d\n",
          settings.aspect, settings.brightness, settings.contrast, settings.deint, settings.subs, settings.prefer,
          settings.leds, settings.ground, settings.hwdec, settings.filter, settings.sort, settings.intro);
  fclose(f);
}

void store_open(const char *user_dir) {
  snprintf(user, sizeof user, "%s", user_dir);
  settings_load();
  char p[1100]; snprintf(p, sizeof p, "%s/resume.txt", user);
  FILE *f = fopen(p, "r"); if (!f) return;
  char line[1200], path[1100]; double pos, dur;
  while (fgets(line, sizeof line, f))
    if (sscanf(line, "%lf\t%lf\t%1099[^\n]", &pos, &dur, path) == 3) store_set_resume(path, pos, dur);
  fclose(f);
  dirty = 0;
}

int store_resume(const char *path, double *pos, double *dur) {
  Res *r = find(path);
  if (!r) return RES_NEW;
  if (pos) *pos = r->pos;
  if (dur) *dur = r->dur;
  return r->pos < 0 ? RES_SEEN : RES_PARTIAL;
}

void store_set_resume(const char *path, double pos, double dur) {
  // Hardly started (30 s, or 5% of a short clip): as if new. In the last 5%, or the last 2 minutes of anything
  // longer than 20 minutes (end titles): seen.
  if (dur > 0 && (pos > dur * .95 || (dur > 1200 && dur - pos < 120))) pos = -1;
  Res *r = find(path);
  if (pos >= 0 && pos < (dur > 0 && dur * .05 < 30 ? dur * .05 : 30)) {
    if (r && r->pos >= 0) { free(r->path); *r = res[--nres]; dirty = 1; }
    return;
  }
  if (!r) {
    if (nres == capres) { capres = capres ? capres * 2 : 64; res = realloc(res, capres * sizeof *res); }
    r = &res[nres++]; r->path = strdup(path);
  }
  r->pos = pos; r->dur = dur; dirty = 1;
}

void store_save(void) {
  if (!dirty) return;
  char p[1100], tmp[1100];
  snprintf(p, sizeof p, "%s/resume.txt", user); snprintf(tmp, sizeof tmp, "%s.tmp", p);
  FILE *f = fopen(tmp, "w"); if (!f) return;
  for (int i = 0; i < nres; i++) fprintf(f, "%.1f\t%.1f\t%s\n", res[i].pos, res[i].dur, res[i].path);
  fclose(f);
  rename(tmp, p);
  dirty = 0;
}
