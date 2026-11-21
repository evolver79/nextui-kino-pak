// Jellyfin on the home network: find the server, log in with Quick Connect, list libraries and items, stream,
// and tell the server how far you got. Every call blocks (network), so the app runs them on a worker thread.
// The plan is in docs/JELLYFIN.md.
#pragma once
#include <stddef.h>

typedef struct {
  char id[40], name[160], type[32];   // Movie, Series, Season, Episode, Folder, BoxSet, CollectionFolder:movies, ...
  char series[120];                   // episodes: the series name
  int folder;                         // has children
  int year, index, season;            // production year; episode / season numbers
  double runtime, pos;                // seconds; resume position from the server
  int played, unplayed;               // seen; unwatched episodes under a series/season
  int has_backdrop;
  char added[20];                     // when it came to the server, "2026-11-02T18:40:11" (films and series)
} JfItem;

int jf_load(const char *user_dir);     // reads the saved login; 1 when logged in
int jf_logged_in(void);
const char *jf_user_name(void);
void jf_logout(void);

int jf_discover(char *server, size_t n);           // asks the LAN (UDP 7359); 1 when a server answered
// Quick Connect: start shows a 6-digit code for the user to approve in the Jellyfin app or web UI.
int jf_qc_start(const char *server, char *code, size_t n);
int jf_qc_poll(void);                              // 1 approved and logged in (saved), 0 not yet, -1 failed

// The Jellyfin root: "Continue watching" and "Next up" (when not empty, ids "resume"/"nextup", type "Special"),
// then the user's libraries. Returns the count (-1 on error).
int jf_views(JfItem **out);
int jf_children(const char *parent_id, const char *parent_type, JfItem **out);
void jf_stream_url(const JfItem *it, char *out, size_t n);
// Asks the server how to play it on the Brick (direct play, or transcoded to H.264/AAC HLS). Fills the url to
// play; returns 1 direct, 2 transcoded, 0 failed. max_bps: above it the server converts to a smaller stream.
// The play session is kept for the playback reports.
int jf_playback(const JfItem *it, char *out, size_t n, int max_bps);
// Subtitle files the server offers for what jf_playback just set up (next to the video, or text tracks a
// transcode can't carry): i from 0 until it returns 0.
int jf_subtitle(int i, const char **url, const char **title, const char **lang);
void jf_poster_url(const JfItem *it, int w, int h, char *out, size_t n);   // 2:3 cover for the grid
void jf_image_url(const JfItem *it, int w, int h, char *out, size_t n);   // 16:9 picture for the detail card
// Playback reports: what 0 = started, 1 = progress, 2 = stopped. Runs on its own thread, returns at once.
void jf_report(int what, const char *item_id, double pos, int paused);
void jf_report_now(int what, const char *item_id, double pos);   // waits for it (quitting): stopped, converter off
