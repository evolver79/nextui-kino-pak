// "Lights follow video": the Brick's LEDs (TrimUI led_anim driver) take the picture's colour while a video plays.
// NextUI's own LED setup is read when Kino starts and written back whenever Kino isn't colouring them, and on quit.
// Writing effect_<zone> starts that zone's effect with the colour/duration/cycles written before it.
#include "kino.h"
#include <stdio.h>
#include <string.h>

#define LED_DIR "/sys/class/led_anim"
static const char *ZONE[] = { "lr", "m", "f1", "f2" };
#define NZONE 4
typedef struct { char effect[16], rgb[16], cycles[16], duration[16]; } Saved;
static Saved saved[NZONE];
static int have_saved, colouring;
static Uint32 shown = 0xffffffff, last_change;

static int rd(const char *key, const char *zone, char *out, size_t n) {
  char p[128]; snprintf(p, sizeof p, LED_DIR "/%s_%s", key, zone);
  FILE *f = fopen(p, "r"); if (!f) return 0;
  int ok = fgets(out, (int)n, f) != NULL;
  fclose(f);
  out[strcspn(out, " \n")] = 0;
  return ok;
}
static void wr(const char *key, const char *zone, const char *val) {
  char p[128]; snprintf(p, sizeof p, LED_DIR "/%s_%s", key, zone);
  FILE *f = fopen(p, "w"); if (!f) return;
  fputs(val, f);
  fclose(f);
}
static void apply(int z, const char *effect, const char *rgb, const char *cycles, const char *duration) {
  char hex[20]; snprintf(hex, sizeof hex, "%s ", rgb);   // the driver wants "RRGGBB " with a space
  wr("effect_rgb_hex", ZONE[z], hex);
  wr("effect_duration", ZONE[z], duration);
  wr("effect_cycles", ZONE[z], cycles);
  wr("effect", ZONE[z], effect);
}

void leds_start(void) {
  have_saved = 1;
  for (int z = 0; z < NZONE; z++) {
    Saved *s = &saved[z];
    if (!rd("effect", ZONE[z], s->effect, sizeof s->effect) || !rd("effect_rgb_hex", ZONE[z], s->rgb, sizeof s->rgb) ||
        !rd("effect_cycles", ZONE[z], s->cycles, sizeof s->cycles) ||
        !rd("effect_duration", ZONE[z], s->duration, sizeof s->duration)) { have_saved = 0; return; }   // no LEDs here
  }
}

void leds_restore(void) {
  if (!have_saved || !colouring) return;
  for (int z = 0; z < NZONE; z++) apply(z, saved[z].effect, saved[z].rgb, saved[z].cycles, saved[z].duration);
  colouring = 0; shown = 0xffffffff;
}

void leds_follow(Uint32 rgb) {
  if (!have_saved) return;
  if (!settings.leds) { leds_restore(); return; }
  Uint32 now = SDL_GetTicks();
  if (rgb == shown || (colouring && now - last_change < 1500)) return;   // calm: at most one change per 1.5 s
  char hex[8]; snprintf(hex, sizeof hex, "%06X", rgb & 0xffffff);
  for (int z = 0; z < NZONE; z++) apply(z, "4", hex, "-1", "1000");    // 4 = static
  colouring = 1; shown = rgb; last_change = now;
}
