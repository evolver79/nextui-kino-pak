// libmpv playback. Video comes out of mpv's software renderer into an SDL streaming texture at the size it will
// roughly have on screen; sound goes through mpv's SDL2 output. mpv stays alive between files (idle).
#include "kino.h"
#include <mpv/client.h>
#include <mpv/render.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mpv_handle *mpv;
static mpv_render_context *rctx;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tw, th, active, pending, stopping, frames, deint_checked;
// A file mpv has open (start to end), and how many "stopped" ends to ignore: opening a file while one is open (a
// reconnect asks the server for a fresh stream and opens it over the old) makes mpv end the old one with STOP, which
// must not count as the new film ending: that marked it watched and went back to the list.
static int loaded, replaced;
static double last_pos, last_dur;   // kept while playing: still known after the stream has broken off
static Uint32 ev_mpv, ev_redraw, accent = 0xff6a2b;
static char sub_label[64];
static char pend_url[12][700], pend_title[12][96], pend_lang[12][16];
static int npend;

// mpv's state as it last told it (observed properties, delivered as events on Kino's own thread). Kino never asks mpv
// and waits: a property read or command waits for mpv's core thread, and when that stalls (a network cut, the video
// engine) the whole UI froze with it, buttons and quitting included.
enum { O_POS = 1, O_DUR, O_WAIT, O_IDLE, O_PAUSE, O_BUFPCT, O_CEND, O_SPEED, O_CDUR, O_DW, O_DH, O_IL, O_SID, O_SLANG, O_SNUM,
       O_TRACKS, R_DEC = 100, R_CODEC };
static struct { double pos, dur, cend, cdur; int wait, idle, pause, il; int64_t bufpct, speed, dw, dh; int subs;
                char sid[16], slang[16], snum[16]; } o;
static void o_reset(void) {   // a file opening: nothing known yet, "opening" (core idle, not paused)
  int pause = o.pause, subs = o.subs;
  memset(&o, 0, sizeof o); o.pos = o.dur = o.cend = o.cdur = -1; o.idle = 1; o.pause = pause; o.subs = subs;
  snprintf(o.sid, sizeof o.sid, "no");
}
static void setp(const char *name, const char *val) {   // set a property without waiting for mpv
  char *v = (char *)val; mpv_set_property_async(mpv, 0, name, MPV_FORMAT_STRING, &v);
}
static void on_property(mpv_event_property *p) {
  int have = p->format != MPV_FORMAT_NONE && p->data;
  double d = have && p->format == MPV_FORMAT_DOUBLE ? *(double *)p->data : -1;
  int64_t i = have && p->format == MPV_FORMAT_INT64 ? *(int64_t *)p->data : 0;
  int f = have && p->format == MPV_FORMAT_FLAG ? *(int *)p->data : 0;
  const char *str = have && p->format == MPV_FORMAT_STRING ? *(char **)p->data : "";
  if (!strcmp(p->name, "time-pos")) o.pos = d;
  else if (!strcmp(p->name, "duration")) o.dur = d;
  else if (!strcmp(p->name, "paused-for-cache")) o.wait = f;
  else if (!strcmp(p->name, "core-idle")) o.idle = f;
  else if (!strcmp(p->name, "pause")) o.pause = f;
  else if (!strcmp(p->name, "cache-buffering-state")) o.bufpct = i;
  else if (!strcmp(p->name, "demuxer-cache-time")) o.cend = d;
  else if (!strcmp(p->name, "cache-speed")) o.speed = i;
  else if (!strcmp(p->name, "demuxer-cache-duration")) o.cdur = d;
  else if (!strcmp(p->name, "dwidth")) o.dw = i;
  else if (!strcmp(p->name, "dheight")) o.dh = i;
  else if (!strcmp(p->name, "sid")) snprintf(o.sid, sizeof o.sid, "%s", have ? str : "no");
  else if (!strcmp(p->name, "current-tracks/sub/lang")) snprintf(o.slang, sizeof o.slang, "%s", str);
  else if (!strcmp(p->name, "current-tracks/sub/id")) snprintf(o.snum, sizeof o.snum, "%s", str);
  else if (!strcmp(p->name, "track-list") && have && p->format == MPV_FORMAT_NODE) {
    mpv_node *n = p->data; int subs = 0;
    if (n->format == MPV_FORMAT_NODE_ARRAY)
      for (int k = 0; k < n->u.list->num; k++) {
        mpv_node *t = &n->u.list->values[k];
        if (t->format != MPV_FORMAT_NODE_MAP) continue;
        for (int j = 0; j < t->u.list->num; j++)
          if (!strcmp(t->u.list->keys[j], "type") && t->u.list->values[j].format == MPV_FORMAT_STRING &&
              !strcmp(t->u.list->values[j].u.string, "sub")) subs++;
      }
    o.subs = subs;
  } else if (!strcmp(p->name, "video-frame-info/interlaced")) {
    o.il = f;   // mpv 0.35 has no deinterlace "auto": on when the picture says it's interlaced
    if (f && active && settings.deint == DEINT_AUTO && !deint_checked) { deint_checked = 1; setp("deinterlace", "yes"); }
  }
}
static void observe(void) {
  static const struct { const char *n; mpv_format f; } p[] = {
    { "time-pos", MPV_FORMAT_DOUBLE }, { "duration", MPV_FORMAT_DOUBLE }, { "paused-for-cache", MPV_FORMAT_FLAG },
    { "core-idle", MPV_FORMAT_FLAG }, { "pause", MPV_FORMAT_FLAG }, { "cache-buffering-state", MPV_FORMAT_INT64 },
    { "demuxer-cache-time", MPV_FORMAT_DOUBLE }, { "cache-speed", MPV_FORMAT_INT64 },
    { "demuxer-cache-duration", MPV_FORMAT_DOUBLE }, { "dwidth", MPV_FORMAT_INT64 }, { "dheight", MPV_FORMAT_INT64 },
    { "video-frame-info/interlaced", MPV_FORMAT_FLAG }, { "sid", MPV_FORMAT_STRING },
    { "current-tracks/sub/lang", MPV_FORMAT_STRING }, { "current-tracks/sub/id", MPV_FORMAT_STRING },
    { "track-list", MPV_FORMAT_NODE } };
  for (unsigned k = 0; k < sizeof p / sizeof *p; k++) mpv_observe_property(mpv, k + 1, p[k].n, p[k].f);
}

// The probe: a thread of its own asks mpv something every 2 s and logs when mpv's core takes long to answer, so a
// stall shows in the log (Kino itself carries on regardless). Holds probe_lock while asking: mpv isn't freed under it.
static pthread_mutex_t probe_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int probe_stop;
static int probe(void *u) {
  (void)u;
  while (!probe_stop) {
    SDL_Delay(2000);
    pthread_mutex_lock(&probe_lock);
    if (mpv && !probe_stop) {
      Uint32 t0 = SDL_GetTicks(); double v;
      mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &v);
      Uint32 took = SDL_GetTicks() - t0;
      if (took > 3000) fprintf(stderr, "probe: mpv's core took %.1f s to answer\n", took / 1000.0);
    }
    pthread_mutex_unlock(&probe_lock);
  }
  return 0;
}

// --- No bars (an aspect choice; Zoom does it too): a film often carries its own bars (a 2.39:1 film in a 16:9 file), dark grey rather than the
// black Kino draws around the picture, so the edge shows two blacks. A few times a second the frame mpv just drew
// (the whole frame: bars and subtitles too) is sampled; a crop that holds for 4 s is taken, one that shows less bar
// within 0.5 s (an IMAX scene opening up to the full frame). Kino then draws only the inside, and moves the
// subtitles up out of the bottom bar by as much. Sides are cut evenly by the smaller bar; the bottom bar is judged
// on its outer fifths only, where subtitles never are. Same rules as Tuner's.
typedef struct { int l, r, t, b; } Crop;
static Crop crop_on, crop_cand; static int crop_n, crop_w, crop_h, crop_margin = -1; static Uint32 crop_at;   // crop_margin: sub-pos set
#define CROP_LIT 3   // samples of 48 that may be bright in a bar (a logo, noise)
static int crop_px_lit(const unsigned char *p) { return p[0] + p[1] + p[2] > 3 * 36; }   // bgr0
static int crop_row_black(const unsigned char *px, int pitch, int y, int x0, int x1) {
  int lit = 0;
  for (int i = 0; i < 48; i++) if (crop_px_lit(px + y * pitch + 4 * (x0 + (x1 - x0 - 1) * i / 47)) && ++lit > CROP_LIT) return 0;
  return 1;
}
static int crop_row_black_edges(const unsigned char *px, int pitch, int y, int w) {   // outer fifths only
  return crop_row_black(px, pitch, y, 0, w / 5) && crop_row_black(px, pitch, y, w - w / 5, w);
}
static int crop_col_black(const unsigned char *px, int pitch, int x, int y0, int y1) {
  int lit = 0;
  for (int i = 0; i < 48; i++) if (crop_px_lit(px + (y0 + (y1 - y0 - 1) * i / 47) * pitch + 4 * x) && ++lit > CROP_LIT) return 0;
  return 1;
}
static int crop_detect(const unsigned char *px, int pitch, int w, int h, Crop *c) {
  int lit = 0;   // a black or nearly black frame (a fade, a night scene) says nothing
  for (int gy = 1; gy < 16; gy++) for (int gx = 1; gx < 16; gx++) lit += crop_px_lit(px + (h * gy / 16) * pitch + 4 * (w * gx / 16));
  if (lit < 8) return -1;
  int t = 0, b = 0, l = 0, r = 0;
  while (t < h / 3 && crop_row_black(px, pitch, t, 0, w)) t++;
  while (b < h / 3 && crop_row_black_edges(px, pitch, h - 1 - b, w)) b++;
  while (l < w / 3 && crop_col_black(px, pitch, l, t, h - b)) l++;
  while (r < w / 3 && crop_col_black(px, pitch, w - 1 - r, t, h - b)) r++;
  int v = t < b ? t : b, hz = l < r ? l : r;
  // Real bars are thinner than a fifth (2.76:1 in a 16:9 file: 18 % top and bottom; 4:3 in 16:9: 12.5 % a side).
  // More is a dark frame with something in the middle (a studio logo, titles): it says nothing about bars.
  if (v > h / 5 || hz > w / 5) return -1;
  if (v < h / 50) v = 0;
  if (hz < w / 50) hz = 0;
  c->t = c->b = v & ~1; c->l = c->r = hz & ~1;
  return 0;
}
static int crop_close(int a, int b, int dim) { int tol = dim / 100 > 2 ? dim / 100 : 2; return abs(a - b) <= tol; }
static void crop_reset(void) { memset(&crop_on, 0, sizeof crop_on); memset(&crop_cand, 0, sizeof crop_cand); crop_n = 0; crop_w = crop_h = 0; }
static void crop_update(const unsigned char *px, int pitch, int w, int h) {
  if (!ASPECT_CROPS(settings.aspect)) { if (crop_on.t || crop_on.l) crop_reset(); }
  else {
    if (w != crop_w || h != crop_h) { crop_reset(); crop_w = w; crop_h = h; }   // another file, or the CRT filter
    Uint32 now = SDL_GetTicks();
    Crop d;
    if (now - crop_at >= 250 && (crop_at = now, crop_detect(px, pitch, w, h, &d) == 0)) {
      if (crop_close(d.l, crop_cand.l, w) && crop_close(d.t, crop_cand.t, h)) crop_n++;
      else { crop_cand = d; crop_n = 1; }
      int less = (crop_cand.l < crop_on.l && !crop_close(crop_cand.l, crop_on.l, w)) ||
                 (crop_cand.t < crop_on.t && !crop_close(crop_cand.t, crop_on.t, h));
      int same = crop_close(crop_cand.l, crop_on.l, w) && crop_close(crop_cand.t, crop_on.t, h);
      if (less ? crop_n >= 2 : !same && crop_n >= 16) {
        crop_on = crop_cand;
        fprintf(stderr, "crop: %dx%d -> %dx%d\n", w, h, w - crop_on.l - crop_on.r, h - crop_on.t - crop_on.b);
      }
    }
  }
  // subtitles: up out of the bottom bar (sub-pos: where they sit, in % of the frame's height from the top; 100 is mpv's
  // bottom. sub-margin-y would be neater, but a loaded .srt keeps the margin it was opened with)
  int pos = 100 - (crop_h ? (crop_on.b * 100 + crop_h - 1) / crop_h : 0);
  if (pos != crop_margin) { crop_margin = pos; char m[16]; snprintf(m, sizeof m, "%d", pos); setp("sub-pos", m); }
}

static void on_wakeup(void *ud) { (void)ud; SDL_Event e = { .type = ev_mpv }; SDL_PushEvent(&e); }
static void on_update(void *ud) { (void)ud; SDL_Event e = { .type = ev_redraw }; SDL_PushEvent(&e); }
static void cmd(const char *a, const char *b, const char *c) {
  const char *v[] = { a, b, c, NULL }; mpv_command_async(mpv, 0, v);
}

int player_init(SDL_Renderer *r, int audio, const char *mpv_dir) {
  ren = r;
  ev_mpv = SDL_RegisterEvents(2); ev_redraw = ev_mpv + 1;
  mpv = mpv_create();
  if (!mpv) return -1;
  mpv_set_option_string(mpv, "vo", "libmpv");
  mpv_set_option_string(mpv, "ao", audio ? "sdl" : "null");
  mpv_set_option_string(mpv, "audio-channels", "stereo");   // the Brick has stereo only: 5.1 would fail to open
  mpv_set_option_string(mpv, "idle", "yes");
  mpv_set_option_string(mpv, "hwdec", "no");
  mpv_set_option_string(mpv, "vd-lavc-threads", "4");
  mpv_set_option_string(mpv, "vd-lavc-fast", "yes");           // skip bit-exact steps the eye won't miss
  mpv_set_option_string(mpv, "vd-lavc-skiploopfilter", "nonkey");
  mpv_set_option_string(mpv, "framedrop", "decoder+vo");
  mpv_set_option_string(mpv, "sws-scaler", "fast-bilinear");
  mpv_set_option_string(mpv, "sub-auto", "fuzzy");               // film.srt / film.en.srt next to the file
  mpv_set_option_string(mpv, "sub-font-size", "54");   // about mpv's default (55); 46 was small on the 3.5" screen
  // libass is built without fontconfig, so it finds no fonts by itself and text subtitles (.srt, Jellyfin's, and text
  // tracks inside a film) came out blank. mpv 0.35 only looks for subtitle fonts in its config folder (fonts/ there),
  // which libmpv has off: so on, at Kino.pak/mpv, which holds Noto Sans (made to be read, not Dot's look) and no
  // mpv.conf. Fonts inside an .mkv still work; picture subtitles (PGS, VobSub) need none.
  mpv_set_option_string(mpv, "config", "yes");
  mpv_set_option_string(mpv, "config-dir", mpv_dir);
  mpv_set_option_string(mpv, "sub-font-provider", "none");
  mpv_set_option_string(mpv, "sub-font", "Noto Sans");
  // A black box behind the text (75 %: a little of the picture shows through), easier to read than an outline alone
  // over a busy picture; the border size is the box's padding
  mpv_set_option_string(mpv, "sub-back-color", "#C0000000");
  mpv_set_option_string(mpv, "sub-border-size", "6");
  mpv_set_option_string(mpv, "osd-level", "0");
  mpv_set_option_string(mpv, "input-default-bindings", "no");
  mpv_set_option_string(mpv, "terminal", "yes");
  // Network streams: FFmpeg reconnects by itself after a short drop (it doesn't by default), a read gives up after
  // 15 s instead of 60 so Kino can open the stream again, and playback waits for 3 s of film before it carries on.
  mpv_set_option_string(mpv, "stream-lavf-o", "reconnect=1,reconnect_streamed=1,reconnect_on_network_error=1,reconnect_delay_max=8");
  mpv_set_option_string(mpv, "network-timeout", "15");
  mpv_set_option_string(mpv, "cache", "yes");
  mpv_set_option_string(mpv, "cache-pause-wait", "3");
  // KINO_CACHE_SECS=10: read only that far ahead (test/net.sh), so a slow or broken network shows within seconds
  // instead of after the minutes of film mpv otherwise keeps
  const char *cs = getenv("KINO_CACHE_SECS");
  if (cs && *cs) {
    mpv_set_option_string(mpv, "cache-secs", cs);
    mpv_set_option_string(mpv, "demuxer-readahead-secs", cs);
    mpv_set_option_string(mpv, "demuxer-max-bytes", "8MiB");
  }
  const char *lv = getenv("KINO_MPV_LOG");   // e.g. "all=warn,vd=v,ffmpeg=v" when looking into a problem
  mpv_set_option_string(mpv, "msg-level", lv && *lv ? lv : "all=warn");
  if (mpv_initialize(mpv) < 0) return -1;
  mpv_render_param params[] = { { MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_SW }, { 0 } };
  if (mpv_render_context_create(&rctx, mpv, params) < 0) return -1;
  observe(); o_reset();
  mpv_set_wakeup_callback(mpv, on_wakeup, NULL);
  SDL_Thread *pt = SDL_CreateThread(probe, "mpv-probe", NULL); if (pt) SDL_DetachThread(pt);
  mpv_render_context_set_update_callback(rctx, on_update, NULL);
  player_apply_settings();
  return 0;
}

// Freeing mpv waits for its core; a stuck core would keep Kino from ever quitting. So it's done on a thread given 3 s.
static SDL_atomic_t freed;
static int free_mpv(void *u) {
  (void)u;
  pthread_mutex_lock(&probe_lock);   // the probe isn't in a call
  if (rctx) mpv_render_context_free(rctx);
  if (mpv) mpv_terminate_destroy(mpv);
  rctx = NULL; mpv = NULL;
  pthread_mutex_unlock(&probe_lock);
  SDL_AtomicSet(&freed, 1);
  return 0;
}
void player_quit(void) {
  probe_stop = 1;
  SDL_Thread *t = SDL_CreateThread(free_mpv, "mpv-free", NULL);
  if (t) {
    SDL_DetachThread(t);
    for (int i = 0; i < 300 && !SDL_AtomicGet(&freed); i++) SDL_Delay(10);
    if (!SDL_AtomicGet(&freed)) fprintf(stderr, "mpv didn't close in 3 s: left as it is\n");
  }
  if (tex) SDL_DestroyTexture(tex);
  tex = NULL;
}

void player_add_sub(const char *url, const char *title, const char *lang) {
  if (npend == 12) return;
  snprintf(pend_url[npend], sizeof pend_url[npend], "%s", url);
  snprintf(pend_title[npend], sizeof pend_title[npend], "%s", title);
  snprintf(pend_lang[npend], sizeof pend_lang[npend], "%s", lang);
  npend++;
}

void player_open(const char *path, double start) {
  char opt[64]; snprintf(opt, sizeof opt, "start=%.1f,pause=no", start > 0 ? start : 0);
  player_apply_settings();
  if (loaded && !stopping) replaced++;   // the open one ends with STOP first (after a stop, that end is the stop's)
  const char *v[] = { "loadfile", path, "replace", opt, NULL };
  mpv_command_async(mpv, 0, v);
  o_reset();
  active = 1; stopping = 0; frames = 0; deint_checked = 0; crop_reset();
  last_pos = start > 0 ? start : 0; last_dur = 0;
  accent = 0xff6a2b;
}

void player_close(void) {
  if (!active) return;
  stopping = 1; active = 0;
  cmd("stop", NULL, NULL);
}

int player_active(void) { return active; }

// 1: the file played to its end. 2: it broke off (an error, or the stream ended well before the film did).
int player_handle(SDL_Event *e) {
  if (e->type == ev_redraw) { pending = 1; return 0; }
  if (e->type != ev_mpv) return 0;
  int ended = 0;
  for (;;) {
    mpv_event *me = mpv_wait_event(mpv, 0);
    if (me->event_id == MPV_EVENT_NONE) break;
    if (me->event_id == MPV_EVENT_START_FILE) loaded = 1;
    if (me->event_id == MPV_EVENT_FILE_LOADED) {
      for (int i = 0; i < npend; i++) {   // the server's subtitle files; "auto" lets slang pick as usual
        const char *v[] = { "sub-add", pend_url[i], "auto", pend_title[i], pend_lang[i], NULL };
        mpv_command_async(mpv, 0, v);
      }
      npend = 0;
      mpv_get_property_async(mpv, R_CODEC, "video-codec", MPV_FORMAT_STRING);
      mpv_get_property_async(mpv, R_DEC, "current-tracks/video/decoder-desc", MPV_FORMAT_STRING);
    }
    if (me->event_id == MPV_EVENT_PROPERTY_CHANGE) on_property(me->data);
    if (me->event_id == MPV_EVENT_GET_PROPERTY_REPLY && me->error >= 0) {
      mpv_event_property *p = me->data;
      if (p->format == MPV_FORMAT_STRING && p->data)
        fprintf(stderr, me->reply_userdata == R_CODEC ? "video: %s" : ", decoder %s\n", *(char **)p->data);
    }
    if (me->event_id == MPV_EVENT_END_FILE) {
      mpv_event_end_file *ef = me->data;
      loaded = 0;
      if (ef->reason == MPV_END_FILE_REASON_STOP && replaced) { replaced--; fprintf(stderr, "end of file: the old stream, replaced\n"); }
      else if (ef->reason == MPV_END_FILE_REASON_STOP && stopping) stopping = 0;
      else if (active && ef->reason != MPV_END_FILE_REASON_REDIRECT) {
        int early = ef->reason == MPV_END_FILE_REASON_EOF && last_dur > 0 && last_pos < last_dur - 20;
        active = 0; ended = ef->reason == MPV_END_FILE_REASON_ERROR || early ? 2 : 1;
        fprintf(stderr, "end of file: reason %d, error %d, at %.0f of %.0f s\n", ef->reason, ef->error, last_pos, last_dur);
      }
    }
  }
  return ended;
}

int player_frame(void) {
  if (!pending) return 0;
  pending = 0;
  if (!(mpv_render_context_update(rctx) & MPV_RENDER_UPDATE_FRAME)) return 0;
  int64_t vw = o.dw, vh = o.dh;
  if (vw <= 0 || vh <= 0) return 0;
  int fw = W, fh = (int)(W * vh / vw);
  if (fh > H) { fh = H; fw = (int)(H * vw / vh); }
  if (fw > vw) { fw = (int)vw; fh = (int)vh; }   // never bigger than the video; SDL scales up
  fw &= ~1; fh &= ~1;
  if (!tex || fw != tw || fh != th) {
    if (tex) SDL_DestroyTexture(tex);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, fw, fh);
    tw = fw; th = fh;
  }
  void *px; int pitch;
  if (SDL_LockTexture(tex, NULL, &px, &pitch) != 0) return 0;
  int size[2] = { tw, th }; size_t stride = pitch;
  mpv_render_param rp[] = { { MPV_RENDER_PARAM_SW_SIZE, size }, { MPV_RENDER_PARAM_SW_FORMAT, "bgr0" },
                            { MPV_RENDER_PARAM_SW_STRIDE, &stride }, { MPV_RENDER_PARAM_SW_POINTER, px }, { 0 } };
  mpv_render_context_render(rctx, rp);
  if (frames++ % 48 == 0) accent = media_accent_of(px, tw, th, pitch, 1);
  crop_update(px, pitch, tw, th);
  SDL_UnlockTexture(tex);
  mpv_render_context_report_swap(rctx);
  return 1;
}

SDL_Texture *player_texture(SDL_Rect *src, SDL_Rect *d) {
  if (!tex) return NULL;
  *src = (SDL_Rect){ 0, 0, tw, th };
  if (ASPECT_CROPS(settings.aspect) && crop_w == tw && crop_h == th)
    *src = (SDL_Rect){ crop_on.l, crop_on.t, tw - crop_on.l - crop_on.r, th - crop_on.t - crop_on.b };
  double a = (double)src->w / src->h;
  if (settings.aspect == ASPECT_4_3) a = 4.0 / 3;
  else if (settings.aspect == ASPECT_16_9) a = 16.0 / 9;
  int dw = W, dh = (int)(W / a + .5);
  if (settings.aspect == ASPECT_STRETCH) dh = H;
  else if (settings.aspect == ASPECT_ZOOM ? dh < H : dh > H) { dh = H; dw = (int)(H * a + .5); }
  *d = (SDL_Rect){ (W - dw) / 2, (H - dh) / 2, dw, dh };
  return tex;
}

double player_pos(void) {
  if (active && o.pos > 0) last_pos = o.pos;
  return last_pos;
}
double player_duration(void) {
  if (active && o.dur > 0) last_dur = o.dur;
  return last_dur;
}
int player_buffering(void) {   // -1: playing normally; else how full the buffer is before playback carries on, 0-100
  if (!active) return -1;
  if (!o.wait && !(o.idle && !o.pause)) return -1;   // core-idle while not paused: opening or seeking, no picture yet
  if (!o.wait) return 101;                           // opening or seeking: waiting for the server, no percentage
  return o.bufpct < 0 ? 0 : o.bufpct > 100 ? 100 : (int)o.bufpct;
}
double player_cache_end(void) { return active ? o.cend : -1; }   // the film time the read-ahead reaches (-1 unknown)
long player_net_speed(void) { return active ? (long)o.speed : 0; }   // bytes/s coming in (mpv's cache-speed)
double player_cache_secs(void) { return active ? o.cdur : -1; }   // how much film is read ahead, in seconds (-1 unknown)
int player_paused(void) { return o.pause; }
void player_pause(int on) { int v = on; o.pause = on; mpv_set_property_async(mpv, 0, "pause", MPV_FORMAT_FLAG, &v); }
void player_seek(double rel) { char s[32]; snprintf(s, sizeof s, "%.1f", rel); cmd("seek", s, "relative"); }
void player_seek_to(double pos) { char s[32]; snprintf(s, sizeof s, "%.1f", pos); cmd("seek", s, "absolute"); }
Uint32 player_accent(void) { return accent; }

void player_apply_settings(void) {
  if (!mpv) return;
  // One FFmpeg filter graph for the picture settings and the filter. All work on the YUV planes (cheap); the CRT
  // look is a softer, smaller picture here plus scanlines that Kino draws on top.
  char g[640] = "";
  size_t n = 0;
  #define ADD(...) n += snprintf(g + n, sizeof g - n, "%s", n ? "," : ""), n += snprintf(g + n, sizeof g - n, __VA_ARGS__)
  // First down to screen size: a 1080p picture has 2-3x the pixels the filters (and mpv after them) need.
  if ((settings.brightness || settings.contrast || settings.filter) && settings.filter != FILTER_CRT)
    ADD("scale=w='min(iw,1024)':h='min(ih,768)':force_original_aspect_ratio=decrease:flags=fast_bilinear");
  // Brightness as a curve on the light (limited range, 16-235) that keeps black black: dark scenes open up without
  // the bars and the night going grey, as an added brightness made them. Contrast stays mpv's eq.
  if (settings.contrast) ADD("eq=contrast=%.2f", 1 + settings.contrast / 50.0);
  if (settings.brightness)
    ADD("lutyuv=y='clip(16+219*pow(clip((val-16)/219,0,1),%.3f),16,235)'", 1 / pow(2, settings.brightness / 50.0));
  switch (settings.filter) {
    case FILTER_SHARPEN: ADD("unsharp=5:5:0.6"); break;
    case FILTER_WARM: ADD("lutyuv=u=val-7:v=val+9"); break;
    case FILTER_COOL: ADD("lutyuv=u=val+9:v=val-6"); break;
    case FILTER_BW: ADD("lutyuv=u=128:v=128,noise=c0s=5:c0f=t"); break;
    case FILTER_CRT: ADD("scale=-2:'min(ih,288)':flags=bilinear,eq=saturation=1.25:contrast=1.08"); break;
  }
  #undef ADD
  char vf[700] = "";
  if (n) snprintf(vf, sizeof vf, "lavfi=[%s]", g);
  setp("vf", vf);
  setp("deinterlace", settings.deint == DEINT_ON ? "yes" : "no");
  if (settings.deint == DEINT_AUTO) deint_checked = 0;
  setp("sub-visibility", settings.subs ? "yes" : "no");
  char sc[16]; snprintf(sc, sizeof sc, "%.2f", SUBSIZE_SCALE[settings.subsize]);
  setp("sub-scale", sc);
  setp("slang", PREFER_LANGS[settings.prefer]);
  setp("vd", settings.hwdec ? "h264_cedar,hevc_cedar" : "");   // next file
}

int player_sub_tracks(void) { return o.subs; }

const char *player_sub_label(void) {
  static const char *names[][2] = { { "eng", "ENGLISH" }, { "en", "ENGLISH" }, { "nor", "NORSK" }, { "nob", "NORSK" },
    { "no", "NORSK" }, { "nb", "NORSK" }, { "swe", "SVENSKA" }, { "sv", "SVENSKA" }, { "dan", "DANSK" }, { "da", "DANSK" },
    { "ger", "DEUTSCH" }, { "deu", "DEUTSCH" }, { "de", "DEUTSCH" }, { "fre", "FRANÇAIS" }, { "fra", "FRANÇAIS" },
    { "fr", "FRANÇAIS" }, { "spa", "ESPAÑOL" }, { "es", "ESPAÑOL" } };
  if (!active) return "AUTO";
  int off = !o.sid[0] || !strcmp(o.sid, "no") || !strcmp(o.sid, "auto");
  if (off) return player_sub_tracks() ? "OFF" : "NONE";
  const char *lang = o.slang[0] ? o.slang : NULL, *id = o.snum[0] ? o.snum : NULL;
  snprintf(sub_label, sizeof sub_label, "TRACK %s", id ? id : "?");
  for (unsigned i = 0; lang && i < sizeof names / sizeof *names; i++)
    if (!strcmp(lang, names[i][0])) { snprintf(sub_label, sizeof sub_label, "%s", names[i][1]); break; }
  if (lang && !strncmp(sub_label, "TRACK", 5)) {
    snprintf(sub_label, sizeof sub_label, "%s", lang);
    for (char *p = sub_label; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
  }
  return sub_label;
}

void player_cycle_sub(int dir) {
  cmd("cycle", "sub", dir < 0 ? "down" : "up");
}

void player_report(const char *file, double secs, long shown, double cpu) {
  int64_t dec_drop = 0, vo_drop = 0; double fps = 0;
  mpv_get_property(mpv, "decoder-frame-drop-count", MPV_FORMAT_INT64, &dec_drop);
  mpv_get_property(mpv, "frame-drop-count", MPV_FORMAT_INT64, &vo_drop);
  mpv_get_property(mpv, "container-fps", MPV_FORMAT_DOUBLE, &fps);
  printf("bench %s: %.1f s, %ld frames shown (%.1f fps, file %.2f fps), dropped %lld decoder + %lld output, CPU %.0f%% of one core\n",
         file, secs, shown, shown / secs, fps, (long long)dec_drop, (long long)vo_drop, 100 * cpu / secs);
}
