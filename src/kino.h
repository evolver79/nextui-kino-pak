// Kino: video player for NextUI on the TrimUI Brick. Shared declarations of the modules.
#pragma once
#include <SDL2/SDL.h>
#include <stdint.h>

#define KINO_VERSION "1.1.0"
#include "dot_tokens.h"
// The device's screen (Brick 1024x768, Smart Pro 1280x720), from dot_screen_detect() at the start of main.
#define W DOT_W
#define H DOT_H
#define DY (768 - H)        // how much shorter than the Brick's: 0 / 48
#define DW (W - 1024)       // how much wider: 0 / 256
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

enum { ASPECT_ORIGINAL, ASPECT_4_3, ASPECT_16_9, ASPECT_STRETCH, ASPECT_ZOOM, ASPECT_NOBARS, NASPECT };   // saved by number: new ones last
#define ASPECT_CROPS(a) ((a) == ASPECT_NOBARS || (a) == ASPECT_ZOOM)   // these cut the film's own black bars (player.c)
enum { DEINT_AUTO, DEINT_ON, DEINT_OFF, NDEINT };
enum { FILTER_NONE, FILTER_SHARPEN, FILTER_WARM, FILTER_COOL, FILTER_BW, FILTER_CRT, NFILTER };
extern const char *FILTER_NAMES[NFILTER], *ASPECT_NAMES[NASPECT];
typedef struct {
  int aspect, brightness, contrast, deint;   // brightness/contrast -50..50 (mpv's -100..100 is too much)
  int subs, ground;                          // ground = Dot light point (test)
  int hwdec;                                 // the video engine for H.264/HEVC (src/cedar)
  int filter;                                // FILTER_*: L1/R1 while playing
  int sort;                                  // SORT_*: the Jellyfin cover grid (SELECT)
  int intro;                                 // 1 once the button screen has been shown on the first start
  int quality;                               // QUALITY_*: the most Jellyfin may send; above it, it converts
  int subsize;                               // SUBSIZE_*: subtitle size (Options > Subtitles > Size)
  char prefer1[4], prefer2[4];               // subtitle languages, first and then (lang.c codes); "": the film's own / none
  char aprefer1[4], aprefer2[4];             // sound languages, the same way ("" first: the film's own)
  int autonext;                              // play the next episode when one ends (after a countdown)
} Settings;
enum { SUBSIZE_S, SUBSIZE_M, SUBSIZE_L, SUBSIZE_XL, NSUBSIZE };
extern const char *SUBSIZE_NAMES[NSUBSIZE];
extern const double SUBSIZE_SCALE[NSUBSIZE];
enum { QUALITY_2, QUALITY_4, QUALITY_8, QUALITY_MAX, NQUALITY };
extern const char *QUALITY_NAMES[NQUALITY];
extern const int QUALITY_BPS[NQUALITY];
enum { SORT_NAME, SORT_ADDED, SORT_UNWATCHED, NSORT };
extern const char *SORT_NAMES[NSORT];
extern Settings settings;
void settings_save(void);

// --- wifi.c: the Wi-Fi signal, read in the background --------------------------------------------------------------
enum { WIFI_UNKNOWN, WIFI_OFF, WIFI_WEAK, WIFI_OK };
void wifi_start(void);
int wifi_state(void);                      // WIFI_*
int wifi_dbm(void);                        // last signal, 0 when not connected
int wifi_drops(void);                      // how often the link has dropped since start (roaming, lost router)
void wifi_describe(char *out, size_t n);   // the link for the log: node, channel, signal, rate, resends since last time
void wifi_power_save(int on);              // off while a Jellyfin film plays: a sleeping radio misses the router's beacons

// --- player.c: libmpv ---------------------------------------------------------------------------------------------
int player_init(SDL_Renderer *r, int audio, const char *mpv_dir);   // mpv_dir: Kino.pak/mpv (fonts/: the subtitle font)
void player_quit(void);
void player_open(const char *path, double start);
void player_add_sub(const char *url, const char *title, const char *lang);   // before player_open: loaded with the file
void player_close(void);                   // stops; the caller saves the position first
int player_active(void);
int player_handle(SDL_Event *e);           // mpv's events; 1 when the file ended, 2 when it broke off
int player_buffering(void);                // -1 playing; 0-100 buffered before carrying on; 101 opening or seeking
double player_cache_secs(void);
long player_net_speed(void);               // bytes/s arriving (mpv's figure: it keeps its last value in an outage)
double player_cache_end(void);             // the film time read ahead to; moving on = data arriving            // film read ahead, s (-1 unknown)
int player_frame(void);                    // renders a new frame into the texture if there is one; 1 = new
SDL_Texture *player_texture(SDL_Rect *src, SDL_Rect *dst);   // the part to draw (black bars cut) and where, with the aspect
double player_pos(void), player_duration(void);
int player_paused(void);
void player_pause(int on);
void player_seek(double rel);
void player_seek_to(double pos);
void player_apply_settings(void);
int player_sub_tracks(void);               // number of subtitle tracks
const char *player_sub_label(void);        // current track: "ENGLISH", "OFF", ...
void player_cycle_sub(int dir);            // by hand: Kino stops choosing the subtitle for this film
const char *player_audio_label(void);      // the sound track: "NORSK · 2/3", "AUTO"
int player_audio_tracks(void);
void player_cycle_audio(int dir);          // by hand: Kino stops choosing the sound for this film
// --- lang.c: language labels ("English", "en-US", "ger", "Norsk") to one code ("eng", "deu", "nor") -----------------
const char *lang_code(const char *s);      // NULL when not a language it knows
const char *lang_label(const char *s);     // the language's own name for Options ("NORSK"), or NULL
int lang_count(void);                      // the languages Kino knows, in the order Options offers them
const char *lang_code_at(int i);
int lang_index(const char *code);          // -1 when not one of them
void player_sub_above(int pct);            // subtitles no lower than pct of the picture's height (100: mpv's own)
Uint32 player_accent(void);                // dominant colour of recent frames
void player_report(const char *file, double secs, long frames, double cpu);   // --bench line

