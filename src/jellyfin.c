// Jellyfin client: see jellyfin.h. HTTP goes through FFmpeg's avio (custom headers, POST bodies), JSON through
// cJSON. The login (server, user id, access token) and a per-device id live in <user>/jellyfin.txt.
#include "jellyfin.h"
#include "vendor/cJSON.h"
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <SDL2/SDL.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static char play_session[80], play_method[16] = "DirectPlay";
typedef struct { char url[700], title[96], lang[16]; } Sub;
static Sub subs[12];
static int nsubs;
static char server[256], user_id[64], user_name[128], token[128], device_id[40], qc_secret[160], path_cfg[1100];

static void save(void) {
  FILE *f = fopen(path_cfg, "w"); if (!f) return;
  fprintf(f, "server=%s\nuser_id=%s\nuser_name=%s\ntoken=%s\ndevice_id=%s\n", server, user_id, user_name, token, device_id);
  fclose(f);
}

int jf_load(const char *user_dir) {
  snprintf(path_cfg, sizeof path_cfg, "%s/jellyfin.txt", user_dir);
  FILE *f = fopen(path_cfg, "r");
  char line[400];
  while (f && fgets(line, sizeof line, f)) {
    line[strcspn(line, "\n")] = 0;
    char *v = strchr(line, '='); if (!v) continue;
    *v++ = 0;
    if (!strcmp(line, "server")) snprintf(server, sizeof server, "%s", v);
    else if (!strcmp(line, "user_id")) snprintf(user_id, sizeof user_id, "%s", v);
    else if (!strcmp(line, "user_name")) snprintf(user_name, sizeof user_name, "%s", v);
    else if (!strcmp(line, "token")) snprintf(token, sizeof token, "%s", v);
    else if (!strcmp(line, "device_id")) snprintf(device_id, sizeof device_id, "%s", v);
  }
  if (f) fclose(f);
  if (!device_id[0]) {   // one id per Brick, so the server lists it as one device
    unsigned char b[8] = { 0 };
    FILE *r = fopen("/dev/urandom", "rb");
    if (!r || fread(b, 1, 8, r) != 8) { Uint32 t = SDL_GetTicks() ^ (Uint32)time(NULL); memcpy(b, &t, 4); }
    if (r) fclose(r);
    for (int i = 0; i < 8; i++) snprintf(device_id + 2 * i, 3, "%02x", b[i]);
    save();
  }
  return jf_logged_in();
}

int jf_logged_in(void) { return server[0] && token[0] && user_id[0]; }
const char *jf_user_name(void) { return user_name; }
void jf_logout(void) { token[0] = user_id[0] = 0; save(); }

// --- HTTP ---------------------------------------------------------------------------------------------------------
// Returns the body (malloc'd, NUL-terminated) or NULL; *err gets FFmpeg's error (e.g. AVERROR_HTTP_UNAUTHORIZED).
static char *http(const char *method, const char *path, const char *body, int *err) {
  char url[1400], hdr[600], *hex = NULL;
  snprintf(url, sizeof url, "%s%s", server, path);
  snprintf(hdr, sizeof hdr, "Authorization: MediaBrowser Client=\"Kino\", Device=\"TrimUI Brick\", DeviceId=\"%s\", "
           "Version=\"0.1\"%s%s%s\r\nAccept: application/json\r\n", device_id, token[0] ? ", Token=\"" : "", token, token[0] ? "\"" : "");
  AVDictionary *o = NULL;
  av_dict_set(&o, "headers", hdr, 0);
  av_dict_set(&o, "timeout", "8000000", 0);   // µs
  if (!strcmp(method, "DELETE")) av_dict_set(&o, "method", "DELETE", 0);
  if (!strcmp(method, "POST")) {
    if (!body) body = "{}";
    size_t n = strlen(body);
    hex = malloc(2 * n + 1);   // FFmpeg's post_data option is binary: given as hex
    for (size_t i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", (unsigned char)body[i]);
    av_dict_set(&o, "post_data", hex, 0);
    av_dict_set(&o, "content_type", "application/json", 0);
    av_dict_set(&o, "method", "POST", 0);
  }
  AVIOContext *io = NULL;
  int r = avio_open2(&io, url, AVIO_FLAG_READ, NULL, &o);
  av_dict_free(&o); free(hex);
  if (err) *err = r;
  if (r < 0) return NULL;
  size_t len = 0, cap = 8192;
  char *out = malloc(cap);
  for (;;) {
    if (len + 4096 + 1 > cap) out = realloc(out, cap *= 2);
    int got = avio_read(io, (unsigned char *)out + len, 4096);
    if (got <= 0) break;
    len += got;
  }
  out[len] = 0;
  avio_closep(&io);
  return out;
}

static cJSON *http_json(const char *method, const char *path, const char *body, int *err) {
  char *b = http(method, path, body, err);
  if (!b) return NULL;
  cJSON *j = cJSON_Parse(b);
  free(b);
  return j;
}

static const char *jstr(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsString(v) ? v->valuestring : ""; }
static double jnum(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsNumber(v) ? v->valuedouble : 0; }
static int jbool(const cJSON *o, const char *k) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k)); }

// --- discovery and Quick Connect ------------------------------------------------------------------------------------
int jf_discover(char *out, size_t n) {
  int s = socket(AF_INET, SOCK_DGRAM, 0), on = 1, found = 0;
  if (s < 0) return 0;
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
  struct timeval tv = { 1, 500000 };
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(7359), .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
  const char *q = "who is JellyfinServer?";
  sendto(s, q, strlen(q), 0, (struct sockaddr *)&a, sizeof a);
  char buf[2048];
  ssize_t got = recv(s, buf, sizeof buf - 1, 0);
  if (got > 0) {
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    if (j && *jstr(j, "Address")) { snprintf(out, n, "%s", jstr(j, "Address")); found = 1; }
    cJSON_Delete(j);
  }
  close(s);
  return found;
}

int jf_qc_start(const char *srv, char *code, size_t n) {
  snprintf(server, sizeof server, "%s", srv);
  size_t l = strlen(server); if (l && server[l - 1] == '/') server[l - 1] = 0;
  token[0] = 0;
  cJSON *j = http_json("POST", "/QuickConnect/Initiate", NULL, NULL);
  if (!j) j = http_json("GET", "/QuickConnect/Initiate", NULL, NULL);   // servers before 10.9
  int ok = j && *jstr(j, "Secret") && *jstr(j, "Code");
  if (ok) { snprintf(qc_secret, sizeof qc_secret, "%s", jstr(j, "Secret")); snprintf(code, n, "%s", jstr(j, "Code")); }
  cJSON_Delete(j);
  return ok;
}

int jf_qc_poll(void) {
  char p[300];
  snprintf(p, sizeof p, "/QuickConnect/Connect?Secret=%s", qc_secret);
  int err = 0;
  cJSON *j = http_json("GET", p, NULL, &err);
  if (!j) return err == AVERROR_HTTP_NOT_FOUND || err == AVERROR_HTTP_UNAUTHORIZED ? -1 : 0;   // expired, or the network
  int approved = jbool(j, "Authenticated");
  cJSON_Delete(j);
  if (!approved) return 0;
  char body[300];
  snprintf(body, sizeof body, "{\"Secret\":\"%s\"}", qc_secret);
  j = http_json("POST", "/Users/AuthenticateWithQuickConnect", body, NULL);
  cJSON *u = j ? cJSON_GetObjectItemCaseSensitive(j, "User") : NULL;
  int ok = j && *jstr(j, "AccessToken") && u;
  if (ok) {
    snprintf(token, sizeof token, "%s", jstr(j, "AccessToken"));
    snprintf(user_id, sizeof user_id, "%s", jstr(u, "Id"));
    snprintf(user_name, sizeof user_name, "%s", jstr(u, "Name"));
    save();
  }
  cJSON_Delete(j);
  return ok ? 1 : -1;
}

// --- items --------------------------------------------------------------------------------------------------------
static int items_from(cJSON *arr, JfItem **out) {
  int n = cJSON_GetArraySize(arr);
  *out = calloc(n ? n : 1, sizeof **out);
  int k = 0;
  cJSON *o;
  cJSON_ArrayForEach(o, arr) {
    JfItem *it = &(*out)[k++];
    snprintf(it->id, sizeof it->id, "%s", jstr(o, "Id"));
    snprintf(it->name, sizeof it->name, "%s", jstr(o, "Name"));
    snprintf(it->type, sizeof it->type, "%s", jstr(o, "Type"));
    if (*jstr(o, "CollectionType"))   // libraries carry their kind: "CollectionFolder:movies", ":tvshows", ...
      snprintf(it->type, sizeof it->type, "CollectionFolder:%.7s", jstr(o, "CollectionType"));
    snprintf(it->series, sizeof it->series, "%s", jstr(o, "SeriesName"));
    it->folder = jbool(o, "IsFolder");
    it->year = (int)jnum(o, "ProductionYear");
    it->index = (int)jnum(o, "IndexNumber");
    it->season = (int)jnum(o, "ParentIndexNumber");
    it->runtime = jnum(o, "RunTimeTicks") / 1e7;
    snprintf(it->added, sizeof it->added, "%.19s", jstr(o, "DateCreated"));   // ISO 8601: sorts as text
    it->has_backdrop = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(o, "BackdropImageTags")) > 0;
    cJSON *ud = cJSON_GetObjectItemCaseSensitive(o, "UserData");
    if (ud) {
      it->pos = jnum(ud, "PlaybackPositionTicks") / 1e7;
      it->played = jbool(ud, "Played");
      it->unplayed = (int)jnum(ud, "UnplayedItemCount");
    }
  }
  return k;
}

static int list(const char *path, JfItem **out) {
  int err = 0;
  cJSON *j = http_json("GET", path, NULL, &err);
  if (!j) { if (err == AVERROR_HTTP_UNAUTHORIZED) jf_logout(); *out = NULL; return -1; }
  int n = items_from(cJSON_GetObjectItemCaseSensitive(j, "Items"), out);
  cJSON_Delete(j);
  return n;
}

int jf_views(JfItem **out) {
  char p[300];
  snprintf(p, sizeof p, "/Users/%s/Views", user_id);
  JfItem *views; int nv = list(p, &views);
  if (nv < 0) { *out = NULL; return -1; }
  JfItem *tmp;
  snprintf(p, sizeof p, "/Users/%s/Items/Resume?MediaTypes=Video&Limit=1", user_id);
  int nr = list(p, &tmp); free(tmp);
  snprintf(p, sizeof p, "/Shows/NextUp?UserId=%s&Limit=1", user_id);
  int nn = list(p, &tmp); free(tmp);
  int n = nv + (nr > 0) + (nn > 0), k = 0;
  *out = calloc(n ? n : 1, sizeof **out);
  if (nr > 0) { JfItem *it = &(*out)[k++]; snprintf(it->id, sizeof it->id, "resume"); snprintf(it->type, sizeof it->type, "Special");
                snprintf(it->name, sizeof it->name, "Continue watching"); it->folder = 1; }
  if (nn > 0) { JfItem *it = &(*out)[k++]; snprintf(it->id, sizeof it->id, "nextup"); snprintf(it->type, sizeof it->type, "Special");
                snprintf(it->name, sizeof it->name, "Next up"); it->folder = 1; }
  memcpy(*out + k, views, nv * sizeof *views);
  free(views);
  return n;
}

int jf_children(const char *parent_id, const char *parent_type, JfItem **out) {
  int episodes = !strcmp(parent_type, "Season") || !strcmp(parent_type, "Series");
  char p[500];
  const char *fields = "Fields=Overview,BackdropImageTags,SeriesName,ProductionYear";
  if (!strcmp(parent_id, "resume")) {
    snprintf(p, sizeof p, "/Users/%s/Items/Resume?MediaTypes=Video&Limit=50&%s", user_id, fields);
    return list(p, out);
  }
  if (!strcmp(parent_id, "nextup")) {
    snprintf(p, sizeof p, "/Shows/NextUp?UserId=%s&Limit=50&%s", user_id, fields);
    return list(p, out);
  }
  // Film and series libraries: every film / series in them (as the Jellyfin apps show), not the raw folders.
  const char *kind = strstr(parent_type, ":movies") ? "Movie" : strstr(parent_type, ":tvshow") ? "Series" : NULL;
  if (kind) {
    snprintf(p, sizeof p, "/Users/%s/Items?ParentId=%s&Recursive=true&IncludeItemTypes=%s&Fields=BackdropImageTags,"
             "ProductionYear,DateCreated&SortBy=SortName&SortOrder=Ascending&Limit=2000", user_id, parent_id, kind);
    return list(p, out);
  }
  snprintf(p, sizeof p, "/Users/%s/Items?ParentId=%s&Fields=Overview,BackdropImageTags,SeriesName,ProductionYear"
           "&SortBy=%s&SortOrder=Ascending&Limit=1000", user_id, parent_id,
           episodes ? "ParentIndexNumber,IndexNumber,SortName" : "IsFolder,SortName");
  return list(p, out);
}

void jf_stream_url(const JfItem *it, char *out, size_t n) {
  snprintf(out, n, "%s/Videos/%s/stream?static=true&MediaSourceId=%s&DeviceId=%s&api_key=%s", server, it->id, it->id,
           device_id, token);
}

// What the Brick plays as it is: the video engine does H.264 and 8-bit HEVC up to 1080p; mpv plays any sound
// (mixed down to stereo). Everything else the server turns into H.264 + stereo AAC over HLS.
static const char *DEVICE_PROFILE =
  "{\"MaxStreamingBitrate\":12000000,\"MaxStaticBitrate\":40000000,\"MusicStreamingTranscodingBitrate\":192000,"
  "\"DirectPlayProfiles\":[{\"Type\":\"Video\",\"Container\":\"mkv,mp4,m4v,mov,webm,avi,ts,m2ts\","
  "\"VideoCodec\":\"h264,hevc\",\"AudioCodec\":\"aac,mp3,ac3,eac3,dts,flac,opus,vorbis,truehd,pcm_s16le,mp2\"}],"
  "\"TranscodingProfiles\":[{\"Type\":\"Video\",\"Container\":\"ts\",\"VideoCodec\":\"h264\","
  "\"AudioCodec\":\"aac\",\"Protocol\":\"hls\",\"Context\":\"Streaming\",\"MaxAudioChannels\":\"2\","
  "\"BreakOnNonKeyFrames\":true,\"MinSegments\":1}],"
  "\"CodecProfiles\":["
  "{\"Type\":\"Video\",\"Codec\":\"hevc\",\"Conditions\":["
  "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoBitDepth\",\"Value\":\"8\",\"IsRequired\":true},"
  "{\"Condition\":\"LessThanEqual\",\"Property\":\"Width\",\"Value\":\"1920\",\"IsRequired\":true},"
  "{\"Condition\":\"EqualsAny\",\"Property\":\"VideoRangeType\",\"Value\":\"SDR\",\"IsRequired\":true}]},"
  "{\"Type\":\"Video\",\"Codec\":\"h264\",\"Conditions\":["
  "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoBitDepth\",\"Value\":\"8\",\"IsRequired\":true},"
  "{\"Condition\":\"LessThanEqual\",\"Property\":\"Width\",\"Value\":\"1920\",\"IsRequired\":true},"
  "{\"Condition\":\"EqualsAny\",\"Property\":\"VideoRangeType\",\"Value\":\"SDR\",\"IsRequired\":true}]}],"
  "\"SubtitleProfiles\":[{\"Format\":\"srt\",\"Method\":\"External\"},{\"Format\":\"ass\",\"Method\":\"External\"},"
  "{\"Format\":\"subrip\",\"Method\":\"Embed\"},{\"Format\":\"ass\",\"Method\":\"Embed\"},"
  "{\"Format\":\"pgssub\",\"Method\":\"Embed\"},{\"Format\":\"dvdsub\",\"Method\":\"Embed\"}]}";

int jf_playback(const JfItem *it, char *out, size_t n, int max_bps) {
  char path[300], *body = malloc(strlen(DEVICE_PROFILE) + 300);
  snprintf(path, sizeof path, "/Items/%s/PlaybackInfo?UserId=%s", it->id, user_id);
  sprintf(body, "{\"DeviceProfile\":%s,\"MaxStreamingBitrate\":%d,\"EnableDirectPlay\":true,"
          "\"EnableDirectStream\":true,\"EnableTranscoding\":true,\"AllowVideoStreamCopy\":true,"
          "\"AllowAudioStreamCopy\":false,\"AutoOpenLiveStream\":true}", DEVICE_PROFILE, max_bps);
  cJSON *j = http_json("POST", path, body, NULL);
  free(body);
  play_session[0] = 0;
  snprintf(play_method, sizeof play_method, "DirectPlay");
  if (!j) { jf_stream_url(it, out, n); return 0; }
  snprintf(play_session, sizeof play_session, "%s", jstr(j, "PlaySessionId"));
  cJSON *ms = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "MediaSources"), 0);
  int how = 1;
  nsubs = 0;
  cJSON *st;
  cJSON_ArrayForEach(st, ms ? cJSON_GetObjectItemCaseSensitive(ms, "MediaStreams") : NULL) {   // subtitle files
    if (strcmp(jstr(st, "Type"), "Subtitle") || strcmp(jstr(st, "DeliveryMethod"), "External") || !*jstr(st, "DeliveryUrl")) continue;
    if (nsubs == 12) break;
    Sub *b = &subs[nsubs++];
    const char *u = jstr(st, "DeliveryUrl");
    snprintf(b->url, sizeof b->url, "%s%s%s", strncmp(u, "http", 4) ? server : "", u,
             strstr(u, "api_key=") ? "" : strchr(u, '?') ? "&api_key=" : "?api_key=");
    if (!strstr(u, "api_key=")) { size_t l = strlen(b->url); snprintf(b->url + l, sizeof b->url - l, "%s", token); }
    snprintf(b->title, sizeof b->title, "%s", *jstr(st, "DisplayTitle") ? jstr(st, "DisplayTitle") : jstr(st, "Title"));
    snprintf(b->lang, sizeof b->lang, "%s", jstr(st, "Language"));
  }
  if (ms && !jbool(ms, "SupportsDirectPlay") && *jstr(ms, "TranscodingUrl")) {
    snprintf(out, n, "%s%s", server, jstr(ms, "TranscodingUrl"));
    snprintf(play_method, sizeof play_method, "Transcode");
    how = 2;
  } else {
    jf_stream_url(it, out, n);
    if (ms && *jstr(ms, "Id")) {   // the media source may differ from the item (versions)
      char *q = strstr(out, "MediaSourceId=");
      if (q) { char rest[400]; char *amp = strchr(q, '&'); snprintf(rest, sizeof rest, "%s", amp ? amp : "");
               snprintf(q, n - (q - out), "MediaSourceId=%s%s", jstr(ms, "Id"), rest); }
    }
  }
  cJSON_Delete(j);
  return how;
}

int jf_subtitle(int i, const char **url, const char **title, const char **lang) {
  if (i < 0 || i >= nsubs) return 0;
  *url = subs[i].url; *title = subs[i].title; *lang = subs[i].lang;
  return 1;
}

void jf_poster_url(const JfItem *it, int w, int h, char *out, size_t n) {
  snprintf(out, n, "%s/Items/%s/Images/Primary?fillWidth=%d&fillHeight=%d&quality=85", server, it->id, w, h);
}

void jf_image_url(const JfItem *it, int w, int h, char *out, size_t n) {
  const char *kind = it->has_backdrop && strcmp(it->type, "Episode") ? "Backdrop" : "Primary";
  snprintf(out, n, "%s/Items/%s/Images/%s?fillWidth=%d&fillHeight=%d&quality=85", server, it->id, kind, w, h);
}

// --- playback reports -----------------------------------------------------------------------------------------------
typedef struct { int what; char id[40]; double pos; int paused; char session[80], method[16]; } Report;
static int report_thread(void *u) {
  Report *r = u;
  static const char *paths[3] = { "/Sessions/Playing", "/Sessions/Playing/Progress", "/Sessions/Playing/Stopped" };
  char body[300];
  snprintf(body, sizeof body, "{\"ItemId\":\"%s\",\"MediaSourceId\":\"%s\",\"PositionTicks\":%lld,\"IsPaused\":%s,"
           "\"PlayMethod\":\"%s\",\"PlaySessionId\":\"%s\",\"CanSeek\":true}", r->id, r->id, (long long)(r->pos * 1e7),
           r->paused ? "true" : "false", r->method, r->session);
  char *b = http("POST", paths[r->what], body, NULL);
  free(b);
  if (r->what == 2 && r->session[0]) {   // stop the server's transcoder for this session too
    char p[300]; snprintf(p, sizeof p, "/Videos/ActiveEncodings?DeviceId=%s&PlaySessionId=%s", device_id, r->session);
    AVDictionary *o = NULL; (void)o;
    b = http("DELETE", p, NULL, NULL); free(b);
  }
  free(r);
  return 0;
}

void jf_report(int what, const char *item_id, double pos, int paused) {
  if (!jf_logged_in() || what < 0 || what > 2) return;
  Report *r = calloc(1, sizeof *r);
  r->what = what; r->pos = pos; r->paused = paused;
  snprintf(r->id, sizeof r->id, "%s", item_id);
  snprintf(r->session, sizeof r->session, "%s", play_session);
  snprintf(r->method, sizeof r->method, "%s", play_method);
  SDL_Thread *t = SDL_CreateThread(report_thread, "jf-report", r);
  if (t) SDL_DetachThread(t); else free(r);
}
