// Kino, first test: plays one file with libmpv on the TrimUI Brick. Video comes out of mpv's software renderer into
// an SDL streaming texture, sized to fit the 1024x768 screen; sound goes through mpv's SDL2 output.
//   kino <file>                    play; A pause, left/right seek 10 s, B quit
//   kino --bench <file> <seconds>  play for that long, then print frames shown, frames dropped and CPU time
#include <SDL2/SDL.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define W 1024
#define H 768

static Uint32 ev_mpv, ev_redraw;
static void on_wakeup(void *ud) { (void)ud; SDL_Event e = { .type = ev_mpv }; SDL_PushEvent(&e); }
static void on_update(void *ud) { (void)ud; SDL_Event e = { .type = ev_redraw }; SDL_PushEvent(&e); }

static double cpu_seconds(void) {
  struct rusage u; getrusage(RUSAGE_SELF, &u);
  return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

int main(int argc, char **argv) {
  int bench = argc >= 4 && !strcmp(argv[1], "--bench");
  const char *file = bench ? argv[2] : argc > 1 ? argv[1] : NULL;
  double bench_s = bench ? atof(argv[3]) : 0;
  if (!file) { fprintf(stderr, "usage: kino <file> | kino --bench <file> <seconds>\n"); return 2; }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
  SDL_Window *win = SDL_CreateWindow("Kino", 0, 0, W, H, SDL_WINDOW_SHOWN);
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
  SDL_RendererInfo ri; SDL_GetRendererInfo(ren, &ri);
  fprintf(stderr, "renderer: %s\n", ri.name);
  SDL_RenderSetLogicalSize(ren, W, H);
  ev_mpv = SDL_RegisterEvents(2); ev_redraw = ev_mpv + 1;

  mpv_handle *mpv = mpv_create();
  mpv_set_option_string(mpv, "vo", "libmpv");
  mpv_set_option_string(mpv, "ao", "sdl");
  mpv_set_option_string(mpv, "hwdec", "no");
  mpv_set_option_string(mpv, "vd-lavc-threads", "4");
  mpv_set_option_string(mpv, "vd-lavc-fast", "yes");           // skip bit-exact steps the eye won't miss
  mpv_set_option_string(mpv, "vd-lavc-skiploopfilter", "nonkey");
  mpv_set_option_string(mpv, "framedrop", "decoder+vo");
  mpv_set_option_string(mpv, "sws-scaler", "fast-bilinear");
  mpv_set_option_string(mpv, "terminal", "yes");
  mpv_set_option_string(mpv, "msg-level", "all=warn");
  if (mpv_initialize(mpv) < 0) { fprintf(stderr, "mpv init failed\n"); return 1; }

  mpv_render_param params[] = { { MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_SW }, { 0 } };
  mpv_render_context *rctx;
  if (mpv_render_context_create(&rctx, mpv, params) < 0) { fprintf(stderr, "mpv render context failed\n"); return 1; }
  mpv_set_wakeup_callback(mpv, on_wakeup, NULL);
  mpv_render_context_set_update_callback(rctx, on_update, NULL);

  const char *cmd[] = { "loadfile", file, NULL };
  mpv_command_async(mpv, 0, cmd);

  // The frame is drawn at the size it will have on screen (aspect kept), then copied to the middle of the screen.
  SDL_Texture *tex = NULL; int tw = 0, th = 0;
  long frames = 0; Uint32 t0 = 0; double c0 = 0;
  int running = 1;
  while (running) {
    SDL_Event e;
    if (!SDL_WaitEventTimeout(&e, 100)) e.type = 0;
    do {
      if (e.type == SDL_QUIT) running = 0;
      else if (e.type == SDL_KEYDOWN || e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_JOYBUTTONDOWN) {
        int k = e.type == SDL_KEYDOWN ? e.key.keysym.sym : -1;
        int b = e.type == SDL_JOYBUTTONDOWN ? e.jbutton.button : -1;
        fprintf(stderr, "input: key %d joy %d\n", k, b);
        if (k == SDLK_ESCAPE || k == SDLK_q || b == 0) running = 0;
        else if (k == SDLK_SPACE || b == 1) { const char *c[] = { "cycle", "pause", NULL }; mpv_command_async(mpv, 0, c); }
        else if (k == SDLK_RIGHT) { const char *c[] = { "seek", "10", NULL }; mpv_command_async(mpv, 0, c); }
        else if (k == SDLK_LEFT) { const char *c[] = { "seek", "-10", NULL }; mpv_command_async(mpv, 0, c); }
      } else if (e.type == ev_mpv) {
        for (;;) {
          mpv_event *me = mpv_wait_event(mpv, 0);
          if (me->event_id == MPV_EVENT_NONE) break;
          if (me->event_id == MPV_EVENT_SHUTDOWN || me->event_id == MPV_EVENT_END_FILE) running = 0;
          if (me->event_id == MPV_EVENT_FILE_LOADED) {
            int64_t vw = 0, vh = 0;
            mpv_get_property(mpv, "width", MPV_FORMAT_INT64, &vw);
            mpv_get_property(mpv, "height", MPV_FORMAT_INT64, &vh);
            char *codec = mpv_get_property_string(mpv, "video-codec");
            fprintf(stderr, "video: %lldx%lld %s\n", (long long)vw, (long long)vh, codec ? codec : "?");
            mpv_free(codec);
            t0 = SDL_GetTicks(); c0 = cpu_seconds(); frames = 0;
          }
        }
      } else if (e.type == ev_redraw) {
        if (!(mpv_render_context_update(rctx) & MPV_RENDER_UPDATE_FRAME)) continue;
        int64_t vw = 0, vh = 0;
        mpv_get_property(mpv, "dwidth", MPV_FORMAT_INT64, &vw);
        mpv_get_property(mpv, "dheight", MPV_FORMAT_INT64, &vh);
        if (vw <= 0 || vh <= 0) continue;
        int fw = W, fh = (int)(W * vh / vw);
        if (fh > H) { fh = H; fw = (int)(H * vw / vh); }
        if (fw > vw) { fw = (int)vw; fh = (int)vh; }   // never draw bigger than the video; SDL scales up
        fw &= ~1; fh &= ~1;
        if (!tex || fw != tw || fh != th) {
          if (tex) SDL_DestroyTexture(tex);
          tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, fw, fh);
          tw = fw; th = fh;
        }
        void *px; int pitch;
        if (SDL_LockTexture(tex, NULL, &px, &pitch) != 0) continue;
        int size[2] = { tw, th }; size_t stride = pitch;
        mpv_render_param rp[] = {
          { MPV_RENDER_PARAM_SW_SIZE, size }, { MPV_RENDER_PARAM_SW_FORMAT, "bgr0" },
          { MPV_RENDER_PARAM_SW_STRIDE, &stride }, { MPV_RENDER_PARAM_SW_POINTER, px }, { 0 } };
        mpv_render_context_render(rctx, rp);
        SDL_UnlockTexture(tex);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        int dw = W, dh = W * th / tw; if (dh > H) { dh = H; dw = H * tw / th; }
        SDL_Rect d = { (W - dw) / 2, (H - dh) / 2, dw, dh };
        SDL_RenderCopy(ren, tex, NULL, &d);
        SDL_RenderPresent(ren);
        mpv_render_context_report_swap(rctx);
        frames++;
      }
    } while (running && SDL_PollEvent(&e));
    if (bench && t0 && SDL_GetTicks() - t0 >= bench_s * 1000) running = 0;
  }
  if (bench && t0) {
    double secs = (SDL_GetTicks() - t0) / 1000.0, cpu = cpu_seconds() - c0;
    int64_t dec_drop = 0, vo_drop = 0; double fps = 0;
    mpv_get_property(mpv, "decoder-frame-drop-count", MPV_FORMAT_INT64, &dec_drop);
    mpv_get_property(mpv, "frame-drop-count", MPV_FORMAT_INT64, &vo_drop);
    mpv_get_property(mpv, "container-fps", MPV_FORMAT_DOUBLE, &fps);
    printf("bench %s: %.1f s, %ld frames shown (%.1f fps, file %.2f fps), dropped %lld decoder + %lld output, CPU %.0f%% of one core\n",
           file, secs, frames, frames / secs, fps, (long long)dec_drop, (long long)vo_drop, 100 * cpu / secs);
  }
  mpv_render_context_free(rctx);
  mpv_terminate_destroy(mpv);
  SDL_Quit();
  return 0;
}
