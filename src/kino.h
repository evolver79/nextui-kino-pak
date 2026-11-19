// Kino: video player for NextUI on the TrimUI Brick. Shared declarations of the modules.
#pragma once
#include <SDL2/SDL.h>
#include <stdint.h>

#define W 1024
#define H 768
#define THUMB_W 428
#define THUMB_H 240
#define POSTER_W 176   // Jellyfin poster grid (Dot: DOT_POSTER_W/H)
#define POSTER_H 264

// --- media.c: file facts and thumbnails, worked out with FFmpeg on a background thread --------------------------
typedef struct {
  int known;                // 1 once probed (0 = not yet, -1 = not a playable video)
  double duration;          // seconds
  int width, height, channels;
  char codec[16];           // "H.264", "HEVC", ...
  char container[8];        // "MKV", "MP4", ...
} MediaInfo;

void media_start(const char *cache_dir);
void media_stop(void);
// What the screen wants now: the thumbnail of one file and the facts of a few. Replaces the previous wish.
void media_want(const char *thumb_path, const char **info_paths, int n);
int media_info(const char *path, MediaInfo *out);                    // 1 when known (out filled)
// The thumbnail as an RGB24 THUMB_W x THUMB_H picture and its accent colour. Returns 1 when ready.
int media_thumb(const char *path, const uint8_t **rgb, Uint32 *accent);
// Posters for the grid (POSTER_W x POSTER_H, RGB24): the visible ones first, then the next page.
void media_want_posters(const char **urls, int n);
int media_poster(const char *url, const uint8_t **rgb);
Uint32 media_accent_of(const uint8_t *px, int w, int h, int pitch, int bgrx);   // vivid dominant colour (RGB24 or BGRX)
void media_label(const MediaInfo *m, char *out, size_t n);         // "720P · HEVC · MKV"
void media_audio_label(const MediaInfo *m, char *out, size_t n);   // "1080P · 5.1"

// --- store.c: resume positions and settings in the user data folder -------------------------------------------
enum { RES_NEW, RES_PARTIAL, RES_SEEN };
void store_open(const char *user_dir);
int store_resume(const char *path, double *pos, double *dur);       // RES_*
void store_set_resume(const char *path, double pos, double dur);    // pos near the end marks it seen
void store_save(void);

enum { ASPECT_ORIGINAL, ASPECT_4_3, ASPECT_16_9, ASPECT_STRETCH, ASPECT_ZOOM, NASPECT };
enum { DEINT_AUTO, DEINT_ON, DEINT_OFF, NDEINT };
enum { FILTER_NONE, FILTER_SHARPEN, FILTER_WARM, FILTER_COOL, FILTER_BW, FILTER_CRT, NFILTER };
extern const char *FILTER_NAMES[NFILTER], *ASPECT_NAMES[NASPECT];
typedef struct {
  int aspect, brightness, contrast, deint;   // brightness/contrast -50..50 (mpv's -100..100 is too much)
  int subs, prefer, leds, ground;            // prefer = index into PREFER_LANGS; ground = Dot light point (test)
  int hwdec;                                 // the video engine for H.264/HEVC (src/cedar)
  int filter;                                // FILTER_*: L1/R1 while playing
} Settings;
extern Settings settings;
extern const char *PREFER_LANGS[], *PREFER_LABELS[];
#define NPREFER 5
void settings_save(void);

// --- player.c: libmpv ---------------------------------------------------------------------------------------------
int player_init(SDL_Renderer *r, int audio);
void player_quit(void);
void player_open(const char *path, double start);
void player_close(void);                   // stops; the caller saves the position first
int player_active(void);
int player_handle(SDL_Event *e);           // mpv's events; returns 1 when the file ended
int player_frame(void);                    // renders a new frame into the texture if there is one; 1 = new
SDL_Texture *player_texture(SDL_Rect *dst);   // where it goes on screen with the current aspect setting
double player_pos(void), player_duration(void);
int player_paused(void);
void player_pause(int on);
void player_seek(double rel);
void player_seek_to(double pos);
void player_apply_settings(void);
int player_sub_tracks(void);               // number of subtitle tracks
const char *player_sub_label(void);        // current track: "ENGLISH", "OFF", ...
void player_cycle_sub(int dir);
Uint32 player_accent(void);                // dominant colour of recent frames
void player_report(const char *file, double secs, long frames, double cpu);   // --bench line

// --- leds.c: "Lights follow video" -------------------------------------------------------------------------------
void leds_start(void);              // remembers NextUI's LED setup
void leds_follow(Uint32 rgb);       // while playing: show this colour (if the setting is on, else NextUI's)
void leds_restore(void);            // NextUI's setup back
