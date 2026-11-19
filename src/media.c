// File facts (length, size, codec, sound) and thumbnails, worked out with FFmpeg on one background thread so the
// browser never waits. Facts are kept in <cache>/info.txt, thumbnails in <cache>/thumbs/<hash>.rgb (accent + RGB24).
// Entries are only removed on the main thread (in media_want), so pointers handed out stay valid for that frame.
#include "kino.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MCAP 2048
#define THUMBS_IN_MEMORY 48   // detail-card pictures and posters
#define NPOSTERS 24
#define NWANT 8
#define DEFAULT_ACCENT 0xff6a2b

typedef struct { char *path; MediaInfo info; uint8_t *rgb; Uint32 accent; int thumb, pw, ph; Uint32 used; } MEnt;
static MEnt ents[MCAP];
static int nents;
static Uint32 tick;
static SDL_mutex *mx;
static SDL_cond *cv;
static SDL_Thread *thread;
static int quitting;
static char want_thumb[1024], want_info[NWANT][1024], want_poster[NPOSTERS][1024];
static int nposter;
static int nwant;
static char dir[1024];

static Uint32 fnv(const char *s, Uint32 h) { while (*s) h = (h ^ (Uint8)*s++) * 16777619u; return h; }

static MEnt *find(const char *path) {
  for (int i = 0; i < nents; i++) if (!strcmp(ents[i].path, path)) return &ents[i];
  return NULL;
}
static MEnt *get(const char *path) {
  MEnt *e = find(path);
  if (e || nents >= MCAP) return e;
  e = &ents[nents++];
  memset(e, 0, sizeof *e);
  e->path = strdup(path);
  return e;
}

// --- FFmpeg ---------------------------------------------------------------------------------------------------
static void codec_label(enum AVCodecID id, char *out, size_t n) {
  const char *s = id == AV_CODEC_ID_H264 ? "H.264" : id == AV_CODEC_ID_HEVC ? "HEVC" : id == AV_CODEC_ID_MPEG4 ? "MPEG-4"
                : id == AV_CODEC_ID_MPEG2VIDEO ? "MPEG-2" : id == AV_CODEC_ID_VP9 ? "VP9" : id == AV_CODEC_ID_AV1 ? "AV1"
                : avcodec_get_name(id);
  snprintf(out, n, "%s", s);
  for (char *p = out; *p; p++) *p = toupper((unsigned char)*p);
}

static void scale_cover(AVFrame *f, uint8_t *dst, int TW, int TH) {
  // Scale to cover TW x TH (display aspect kept), then cut the middle out.
  double sar = f->sample_aspect_ratio.num > 0 ? av_q2d(f->sample_aspect_ratio) : 1;
  double aspect = f->width * sar / f->height, want = (double)TW / TH;
  int sw = aspect > want ? (int)(TH * aspect + .5) : TW, sh = aspect > want ? TH : (int)(TW / aspect + .5);
  sw = sw < TW ? TW : sw; sh = sh < TH ? TH : sh;
  struct SwsContext *sws = sws_getContext(f->width, f->height, f->format, sw, sh, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
  if (!sws) return;
  uint8_t *tmp = malloc((size_t)sw * sh * 3);
  uint8_t *dp[4] = { tmp }; int dl[4] = { sw * 3 };
  sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dp, dl);
  sws_freeContext(sws);
  int ox = (sw - TW) / 2, oy = (sh - TH) / 2;
  for (int y = 0; y < TH; y++) memcpy(dst + y * TW * 3, tmp + ((y + oy) * sw + ox) * 3, TW * 3);
  free(tmp);
}

// Facts, and with rgb != NULL a picture from 10% in (at most 5 minutes in).
static void probe(const char *path, MediaInfo *mi, uint8_t **rgb, int TW, int TH) {
  memset(mi, 0, sizeof *mi);
  mi->known = -1;
  AVFormatContext *fc = NULL;
  if (avformat_open_input(&fc, path, NULL, NULL) < 0) return;
  if (avformat_find_stream_info(fc, NULL) < 0) { avformat_close_input(&fc); return; }
  const AVCodec *dec = NULL;
  int vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
  if (vs < 0) { avformat_close_input(&fc); return; }
  AVCodecParameters *par = fc->streams[vs]->codecpar;
  mi->known = 1;
  mi->duration = fc->duration > 0 ? fc->duration / (double)AV_TIME_BASE : 0;
  mi->width = par->width; mi->height = par->height;
  codec_label(par->codec_id, mi->codec, sizeof mi->codec);
  int as = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, vs, NULL, 0);
  if (as >= 0) mi->channels = fc->streams[as]->codecpar->ch_layout.nb_channels;
  const char *dot = strrchr(path, '.');
  snprintf(mi->container, sizeof mi->container, "%s", dot ? dot + 1 : "");
  for (char *p = mi->container; *p; p++) *p = toupper((unsigned char)*p);

  if (rgb && dec) {
    AVCodecContext *cc = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(cc, par);
    cc->thread_count = 2;
    if (avcodec_open2(cc, dec, NULL) == 0) {
      double at = mi->duration * .1; if (at > 300) at = 300;
      if (at > 1) av_seek_frame(fc, -1, (int64_t)(at * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
      AVPacket *pkt = av_packet_alloc(); AVFrame *f = av_frame_alloc();
      int got = 0;
      for (int n = 0; n < 400 && !got && av_read_frame(fc, pkt) >= 0; n++) {
        if (pkt->stream_index == vs && avcodec_send_packet(cc, pkt) >= 0 && avcodec_receive_frame(cc, f) == 0) got = 1;
        av_packet_unref(pkt);
      }
      if (!got) { avcodec_send_packet(cc, NULL); got = avcodec_receive_frame(cc, f) == 0; }
      if (got) { *rgb = calloc(TW * TH, 3); scale_cover(f, *rgb, TW, TH); }
      av_frame_free(&f); av_packet_free(&pkt);
    }
    avcodec_free_context(&cc);
  }
  avformat_close_input(&fc);
}

// --- covers made from the video ---------------------------------------------------------------------------------
// "frame:<path>": four frames spread through the film; the busiest, most colourful one that isn't dark is kept.
// The cover is that frame whole, across the middle, on a blurred and darkened copy of itself.
static double frame_score(AVFrame *f) {
  uint8_t px[32 * 18 * 3];
  scale_cover(f, px, 32, 18);
  double sum = 0, sum2 = 0, sat = 0;
  for (int i = 0; i < 32 * 18; i++) {
    const uint8_t *p = px + i * 3;
    double l = .299 * p[0] + .587 * p[1] + .114 * p[2];
    int mxc = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
    int mnc = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
    sum += l; sum2 += l * l; sat += mxc - mnc;
  }
  double n = 32 * 18, mean = sum / n, sd = sqrt(fmax(0, sum2 / n - mean * mean));
  double score = sd + sat / n * .5;
  if (mean < 35) score *= mean / 35;    // black screens and fades
  if (mean > 225) score *= .3;          // white title cards
  return score;
}

static void blur_dark(const uint8_t *src, int sw, int sh, uint8_t *dst, int w, int h, double dim) {   // bilinear up
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      double fx = (x + .5) * sw / w - .5, fy = (y + .5) * sh / h - .5;
      int x0 = fx < 0 ? 0 : (int)fx, y0 = fy < 0 ? 0 : (int)fy, x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
      double ax = fx - x0, ay = fy - y0; if (ax < 0) ax = 0; if (ay < 0) ay = 0;
      for (int c = 0; c < 3; c++) {
        double v = (src[(y0 * sw + x0) * 3 + c] * (1 - ax) + src[(y0 * sw + x1) * 3 + c] * ax) * (1 - ay)
                 + (src[(y1 * sw + x0) * 3 + c] * (1 - ax) + src[(y1 * sw + x1) * 3 + c] * ax) * ay;
        dst[(y * w + x) * 3 + c] = (uint8_t)(v * dim);
      }
    }
}

static uint8_t *frame_cover(const char *path, int W_, int H_) {
  AVFormatContext *fc = NULL;
  if (avformat_open_input(&fc, path, NULL, NULL) < 0) return NULL;
  uint8_t *out = NULL;
  const AVCodec *dec = NULL;
  int vs = avformat_find_stream_info(fc, NULL) < 0 ? -1 : av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
  AVCodecContext *cc = vs >= 0 && dec ? avcodec_alloc_context3(dec) : NULL;
  if (cc) {
    avcodec_parameters_to_context(cc, fc->streams[vs]->codecpar);
    cc->thread_count = 2;
  }
  if (cc && avcodec_open2(cc, dec, NULL) == 0) {
    double dur = fc->duration > 0 ? fc->duration / (double)AV_TIME_BASE : 0, best_score = -1;
    AVFrame *best = NULL, *f = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    for (int k = 0; k < 4; k++) {
      if (dur > 4) av_seek_frame(fc, -1, (int64_t)(dur * (.15 + .2 * k) * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
      else if (k) break;
      avcodec_flush_buffers(cc);
      int got = 0;
      for (int n = 0; n < 400 && !got && av_read_frame(fc, pkt) >= 0; n++) {
        if (pkt->stream_index == vs && avcodec_send_packet(cc, pkt) >= 0 && avcodec_receive_frame(cc, f) == 0) got = 1;
        av_packet_unref(pkt);
      }
      if (!got) continue;
      double sc = frame_score(f);
      if (sc > best_score) { best_score = sc; av_frame_free(&best); best = av_frame_clone(f); }
      av_frame_unref(f);
    }
    if (best) {
      out = malloc((size_t)W_ * H_ * 3);
      uint8_t small[16 * 24 * 3];   // the backdrop: the frame filling the cover, tiny, scaled back up dark
      scale_cover(best, small, 16, 24);
      blur_dark(small, 16, 24, out, W_, H_, .45);
      double sar = best->sample_aspect_ratio.num > 0 ? av_q2d(best->sample_aspect_ratio) : 1;
      int fh = (int)(W_ * best->height / (best->width * sar) + .5);   // the whole frame, full width
      if (fh > H_) fh = H_;
      if (fh > 0) {
        uint8_t *mid = malloc((size_t)W_ * fh * 3);
        scale_cover(best, mid, W_, fh);
        memcpy(out + (size_t)(H_ - fh) / 2 * W_ * 3, mid, (size_t)W_ * fh * 3);
        free(mid);
      }
      av_frame_free(&best);
    }
    av_frame_free(&f); av_packet_free(&pkt);
  }
  avcodec_free_context(&cc);
  avformat_close_input(&fc);
  return out;
}

Uint32 media_accent_of(const uint8_t *px, int w, int h, int pitch, int bgrx) {
  // Hue of the saturated, bright parts (weighted), shown vivid; grey pictures keep the default orange.
  int bpp = bgrx ? 4 : 3;
  double cx = 0, cy = 0, wsum = 0;
  for (int y = h / 36; y < h; y += h / 18 > 0 ? h / 18 : 1)
    for (int x = w / 64; x < w; x += w / 32 > 0 ? w / 32 : 1) {
      const uint8_t *p = px + y * pitch + x * bpp;
      double r = (bgrx ? p[2] : p[0]) / 255., g = p[1] / 255., b = (bgrx ? p[0] : p[2]) / 255.;
      double mx_ = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), c = mx_ - mn;
      if (c < .08) continue;
      double hue = mx_ == r ? fmod((g - b) / c, 6) : mx_ == g ? (b - r) / c + 2 : (r - g) / c + 4;
      double wgt = c * mx_;
      cx += cos(hue * M_PI / 3) * wgt; cy += sin(hue * M_PI / 3) * wgt; wsum += wgt;
    }
  if (wsum < 1.5 || hypot(cx, cy) < wsum * .25) return DEFAULT_ACCENT;
  double hue = atan2(cy, cx) * 3 / M_PI; if (hue < 0) hue += 6;
  double s = .72, v = .96, c = v * s, x = c * (1 - fabs(fmod(hue, 2) - 1)), m = v - c, r, g, b;
  int i = (int)hue;
  r = i == 0 || i == 5 ? c : i == 1 || i == 4 ? x : 0;
  g = i == 1 || i == 2 ? c : i == 0 || i == 3 ? x : 0;
  b = i == 3 || i == 4 ? c : i == 2 || i == 5 ? x : 0;
  return (Uint32)((r + m) * 255) << 16 | (Uint32)((g + m) * 255) << 8 | (Uint32)((b + m) * 255);
}

// --- disk cache -----------------------------------------------------------------------------------------------
static void thumb_file(const char *path, int w, int h, char *out, size_t n) {
  struct stat st; Uint32 k = 0;
  if (stat(!strncmp(path, "frame:", 6) ? path + 6 : path, &st) == 0) k = (Uint32)st.st_mtime ^ (Uint32)st.st_size;
  snprintf(out, n, "%s/thumbs/%08x%08x-%dx%d.rgb", dir, fnv(path, 2166136261u), k, w, h);
}

static void info_load(void) {
  char p[1100]; snprintf(p, sizeof p, "%s/info.txt", dir);
  FILE *f = fopen(p, "r"); if (!f) return;
  char line[1400];
  while (fgets(line, sizeof line, f)) {
    MediaInfo m = { 1 }; char path[1100];
    line[strcspn(line, "\n")] = 0;
    if (sscanf(line, "%lf\t%d\t%d\t%d\t%15[^\t]\t%7[^\t]\t%1099[^\n]", &m.duration, &m.width, &m.height, &m.channels,
               m.codec, m.container, path) == 7) {
      MEnt *e = get(path); if (e) e->info = m;
    }
  }
  fclose(f);
}
static void info_append(const char *path, const MediaInfo *m) {
  char p[1100]; snprintf(p, sizeof p, "%s/info.txt", dir);
  FILE *f = fopen(p, "a"); if (!f) return;
  fprintf(f, "%.3f\t%d\t%d\t%d\t%s\t%s\t%s\n", m->duration, m->width, m->height, m->channels,
          m->codec[0] ? m->codec : "-", m->container[0] ? m->container : "-", path);
  fclose(f);
}

// --- worker ---------------------------------------------------------------------------------------------------
static int worker(void *u) {
  (void)u;
  SDL_LockMutex(mx);
  while (!quitting) {
    char job[1024]; int kind = 0;
    int pw = THUMB_W, ph = THUMB_H;
    if (want_thumb[0]) { MEnt *e = find(want_thumb); if (!e || !e->thumb) { snprintf(job, sizeof job, "%s", want_thumb); kind = 2; } }
    for (int i = 0; !kind && i < nposter; i++) {
      MEnt *e = find(want_poster[i]);
      if (!e || !e->thumb) { snprintf(job, sizeof job, "%s", want_poster[i]); kind = 2; pw = POSTER_W; ph = POSTER_H; }
    }
    for (int i = 0; !kind && i < nwant; i++) {
      MEnt *e = find(want_info[i]);
      if (!e || !e->info.known) { snprintf(job, sizeof job, "%s", want_info[i]); kind = 1; }
    }
    if (!kind) { SDL_CondWait(cv, mx); continue; }
    MEnt *had = find(job);
    int frame = !strncmp(job, "frame:", 6);   // a cover made from the video (no facts of its own)
    int need_info = !frame && (!had || !had->info.known);
    SDL_UnlockMutex(mx);

    MediaInfo mi = { 0 }; uint8_t *rgb = NULL; Uint32 acc = DEFAULT_ACCENT;
    char tf[1200]; FILE *f = NULL;
    if (kind == 2) { thumb_file(job, pw, ph, tf, sizeof tf); f = fopen(tf, "rb"); }
    if (f) {   // picture from the disk cache
      rgb = malloc(pw * ph * 3);
      if (fread(&acc, 4, 1, f) != 1 || fread(rgb, pw * ph * 3, 1, f) != 1) { free(rgb); rgb = NULL; }
      fclose(f);
    }
    if (frame && !rgb) {
      rgb = frame_cover(job + 6, pw, ph);
      if (rgb) {
        acc = media_accent_of(rgb, pw, ph, pw * 3, 0);
        if ((f = fopen(tf, "wb"))) { fwrite(&acc, 4, 1, f); fwrite(rgb, pw * ph * 3, 1, f); fclose(f); }
      }
    } else if (need_info || (kind == 2 && !rgb)) {
      probe(job, &mi, kind == 2 && !rgb ? &rgb : NULL, pw, ph);
      if (kind == 2 && rgb && mi.known == 1) {
        acc = media_accent_of(rgb, pw, ph, pw * 3, 0);
        if ((f = fopen(tf, "wb"))) { fwrite(&acc, 4, 1, f); fwrite(rgb, pw * ph * 3, 1, f); fclose(f); }
      }
    }

    SDL_LockMutex(mx);
    MEnt *e = get(job);
    if (e) {
      if (need_info) { e->info = mi; if (mi.known == 1) info_append(job, &mi); }
      if (kind == 2) { e->rgb = rgb; e->accent = acc; e->thumb = rgb ? 1 : -1; e->used = tick; e->pw = pw; e->ph = ph; }
      else free(rgb);
      if (e->info.known == -1 && kind == 2) e->thumb = -1;
    } else free(rgb);
  }
  SDL_UnlockMutex(mx);
  return 0;
}

void media_start(const char *cache_dir) {
  snprintf(dir, sizeof dir, "%s", cache_dir);
  mkdir(dir, 0755);
  char p[1100]; snprintf(p, sizeof p, "%s/thumbs", dir); mkdir(p, 0755);
  av_log_set_level(AV_LOG_QUIET);
  info_load();
  mx = SDL_CreateMutex(); cv = SDL_CreateCond();
  thread = SDL_CreateThread(worker, "media", NULL);
}

void media_stop(void) {
  if (!thread) return;
  SDL_LockMutex(mx); quitting = 1; SDL_CondSignal(cv); SDL_UnlockMutex(mx);
  SDL_WaitThread(thread, NULL); thread = NULL;
}

void media_want(const char *thumb_path, const char **info_paths, int n) {
  SDL_LockMutex(mx);
  tick++;
  snprintf(want_thumb, sizeof want_thumb, "%s", thumb_path ? thumb_path : "");
  nwant = n < NWANT ? n : NWANT;
  for (int i = 0; i < nwant; i++) snprintf(want_info[i], sizeof want_info[i], "%s", info_paths[i]);
  if (want_thumb[0]) { MEnt *e = find(want_thumb); if (e) e->used = tick; }
  for (int i = 0; i < nposter; i++) { MEnt *e = find(want_poster[i]); if (e) e->used = tick; }
  // Keep at most THUMBS_IN_MEMORY pictures; the oldest go (they come back from disk quickly).
  int held = 0;
  for (int i = 0; i < nents; i++) held += ents[i].rgb != NULL;
  while (held > THUMBS_IN_MEMORY) {
    MEnt *old = NULL;
    for (int i = 0; i < nents; i++) if (ents[i].rgb && (!old || ents[i].used < old->used)) old = &ents[i];
    free(old->rgb); old->rgb = NULL; old->thumb = 0; held--;
  }
  SDL_CondSignal(cv);
  SDL_UnlockMutex(mx);
}

void media_want_posters(const char **urls, int n) {
  SDL_LockMutex(mx);
  nposter = n < NPOSTERS ? n : NPOSTERS;
  for (int i = 0; i < nposter; i++) {
    snprintf(want_poster[i], sizeof want_poster[i], "%s", urls[i]);
    MEnt *e = find(want_poster[i]); if (e) e->used = tick;
  }
  SDL_CondSignal(cv);
  SDL_UnlockMutex(mx);
}

int media_poster(const char *url, const uint8_t **rgb) {
  SDL_LockMutex(mx);
  MEnt *e = find(url);
  int ok = e && e->rgb && e->pw == POSTER_W;
  if (ok) *rgb = e->rgb;
  SDL_UnlockMutex(mx);
  return ok;
}

int media_info(const char *path, MediaInfo *out) {
  SDL_LockMutex(mx);
  MEnt *e = find(path);
  int ok = e && e->info.known == 1;
  if (ok) *out = e->info;
  SDL_UnlockMutex(mx);
  return ok;
}

int media_thumb(const char *path, const uint8_t **rgb, Uint32 *accent) {
  SDL_LockMutex(mx);
  MEnt *e = find(path);
  int ok = e && e->rgb && e->pw == THUMB_W;
  if (ok) { *rgb = e->rgb; *accent = e->accent; }
  SDL_UnlockMutex(mx);
  return ok;
}

void media_label(const MediaInfo *m, char *out, size_t n) {
  char res[16];
  if (m->width >= 3000 || m->height >= 2000) snprintf(res, sizeof res, "4K");
  else if (m->width >= 1800 || m->height >= 1000) snprintf(res, sizeof res, "1080P");
  else if (m->width >= 1200 || m->height >= 700) snprintf(res, sizeof res, "720P");
  else snprintf(res, sizeof res, "%dP", m->height);
  snprintf(out, n, "%s · %s · %s", res, m->codec, m->container);
}

void media_audio_label(const MediaInfo *m, char *out, size_t n) {
  char res[32]; media_label(m, res, sizeof res);
  *strchr(res, ' ') = 0;
  const char *ch = m->channels == 6 ? "5.1" : m->channels == 8 ? "7.1" : m->channels == 2 ? "STEREO" : m->channels == 1 ? "MONO" : NULL;
  if (ch) snprintf(out, n, "%s · %s", res, ch);
  else if (m->channels) snprintf(out, n, "%s · %d CH", res, m->channels);
  else snprintf(out, n, "%s", res);
}
