// h264_cedar / hevc_cedar: FFmpeg decoders on the Allwinner video engine of the TrimUI Brick (A133), through the
// CedarC libraries in /usr/lib, loaded at run time. Copied into FFmpeg's libavcodec by build/deps.sh; mpv picks them
// with --vd=lavc:h264_cedar,lavc:hevc_cedar and falls back to the normal decoders when init fails (no engine,
// 10-bit, too big). If the engine fails later in a file, this decoder carries on in software itself.
// The engine part is Tuner's hwdec.c (CedarC 1.3.0 struct layout, checked against the library: memops at byte
// 136, VE ops at 152, proc info at 180).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "libavutil/opt.h"
#include "avcodec.h"
#include "bsf.h"
#include "codec_internal.h"
#include "decode.h"

// --- CedarC ------------------------------------------------------------------------------------------------------
typedef struct { int eCodecFormat, nWidth, nHeight, nFrameRate, nFrameDuration, nAspectRatio, bIs3DStream, nCodecSpecificDataLen;
  char *pCodecSpecificData; int bSecureStreamFlag, bSecureStreamFlagLevel1, bIsFramePackage, h265ReferencePictureNum, bReOpenEngine, bIsFrameCtsTestFlag; } VideoStreamInfo;
typedef struct { int bScaleDownEn, bRotationEn, bSecOutputEn, nHorizonScaleDownRatio, nVerticalScaleDownRatio, nSDWidth, nSDHeight, bAnySizeSD,
  nSecHorizonScaleDownRatio, nSecVerticalScaleDownRatio, nRotateDegree, bThumbnailMode, eOutputPixelFormat, eSecOutputPixelFormat, bNoBFrames,
  bDisable3D, bSupportMaf, bDispErrorFrame, nVbvBufferSize, nFrameBufferNum, bSecureosEn, bGpuBufValid, nAlignStride, bIsSoftDecoderFlag,
  bVirMallocSbm, bSupportPallocBufBeforeDecode, nDeInterlaceHoldingFrameBufferNum, nDisplayHoldingFrameBufferNum, nRotateHoldingFrameBufferNum,
  nDecodeSmoothFrameBufferNum, bIsTvStream, nLbcLossyComMod; unsigned bIsLossy, bRcEn;
  void *memops; int eCtlAfbcMode, eCtlIptvMode; void *veOpsS; void *pVeOpsSelf; int bConvertVp910bitTo8bit; unsigned nVeFreq;
  int bCalledByOmxFlag, bSetProcInfoEnable, nSetProcInfoFreq, nChannelNum, nSupportMaxWidth, nSupportMaxHeight, commonConfigFlag, bATMFlag; } VConfig;
_Static_assert(__builtin_offsetof(VConfig, memops) == 136, "CedarC 1.3 VConfig: memops at 136");
_Static_assert(__builtin_offsetof(VConfig, veOpsS) == 152, "CedarC 1.3 VConfig: veOpsS at 152");
_Static_assert(__builtin_offsetof(VConfig, bSetProcInfoEnable) == 180, "CedarC 1.3 VConfig: proc info at 180");
typedef struct { char *pData; int nLength; int64_t nPts, nPcr; int bIsFirstPart, bIsLastPart, nID, nStreamIndex, bValid; unsigned bVideoInfoFlag; void *pVideoInfo; } VideoStreamDataInfo;
typedef struct { int nID, nStreamIndex, ePixelFormat, nWidth, nHeight, nLineStride, nTopOffset, nLeftOffset, nBottomOffset, nRightOffset,
  nFrameRate, nAspectRatio, bIsProgressive, bTopFieldFirst, bRepeatTopField; int64_t nPts, nPcr; char *pData0, *pData1, *pData2, *pData3; } VideoPicture;
typedef struct { int (*open)(void); int (*open2)(void *, void *); void (*close)(void); int (*total_size)(void); void *(*palloc)(int, void *, void *);
  void *(*palloc_no_cache)(int, void *, void *); void (*pfree)(void *, void *, void *); void (*flush_cache)(void *, int); } ScMemOpsS;
enum { FMT_H264 = 0x115, FMT_H265 = 0x116, PIX_YV12 = 4, R_NOFB = 4, R_NOBS = 5, R_RESCHANGE = 6 };

static void (*AddVDPlugin)(void);
static void *(*CreateVideoDecoder)(void);
static void (*DestroyVideoDecoder)(void *);
static int (*InitializeVideoDecoder)(void *, VideoStreamInfo *, VConfig *);
static int (*DecodeVideoStream)(void *, int, int, int, int64_t);
static int (*RequestVideoStreamBuffer)(void *, int, char **, int *, char **, int *, int);
static int (*SubmitVideoStreamData)(void *, VideoStreamDataInfo *, int);
static VideoPicture *(*RequestPicture)(void *, int);
static int (*ReturnPicture)(void *, VideoPicture *);
static ScMemOpsS *(*MemAdapterGetOpsS)(void);
static ScMemOpsS *mem;
static atomic_int busy;   // the video engine is one: one decoder at a time

typedef struct { void *dec; AVBSFContext *bsf; AVRational tb; AVPacket *tmp; int failed, in, out; } HwDec;

static int hw_available(void) {
  static int state = -1;
  if (state >= 0) return state;
  state = 0;
  const char *libs[] = { "libcdc_base.so", "libVE.so", "libMemAdapter.so", "libvideoengine.so", "libvdecoder.so" };
  for (int i = 0; i < 5; i++) if (!dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL)) return 0;
  #define SYM(n) do { *(void **)&n = dlsym(RTLD_DEFAULT, #n); if (!n) return 0; } while (0)
  SYM(AddVDPlugin); SYM(CreateVideoDecoder); SYM(DestroyVideoDecoder); SYM(InitializeVideoDecoder); SYM(DecodeVideoStream);
  SYM(RequestVideoStreamBuffer); SYM(SubmitVideoStreamData); SYM(RequestPicture); SYM(ReturnPicture); SYM(MemAdapterGetOpsS);
  #undef SYM
  AddVDPlugin();
  if (!(mem = MemAdapterGetOpsS()) || mem->open() < 0) return 0;
  state = 1;
  return 1;
}

static void hw_close(HwDec *d) {
  if (!d) return;
  if (d->dec) DestroyVideoDecoder(d->dec);
  av_bsf_free(&d->bsf); av_packet_free(&d->tmp);
  av_free(d);
  atomic_store(&busy, 0);
}

static HwDec *hw_open(const AVCodecParameters *par, AVRational time_base, double fps) {
  int h265 = par->codec_id == AV_CODEC_ID_HEVC;
  if (par->format != AV_PIX_FMT_NONE && par->format != AV_PIX_FMT_YUV420P && par->format != AV_PIX_FMT_YUVJ420P) return NULL;   // 10-bit, 4:2:2
  // The pixel format may not be known yet when mpv opens the decoder: the profile tells 10-bit too.
  if (h265 && (par->profile == FF_PROFILE_HEVC_MAIN_10 || par->profile == FF_PROFILE_HEVC_REXT)) return NULL;
  if (!h265 && par->profile >= FF_PROFILE_H264_HIGH_10 && par->profile != FF_PROFILE_H264_CONSTRAINED_BASELINE) return NULL;
  if (par->bits_per_raw_sample > 8) return NULL;
  if (par->width > 4096 || par->height > 2304) return NULL;
  if (!hw_available()) return NULL;
  for (int i = 0, z = 0; !atomic_compare_exchange_strong(&busy, &z, 1); i++, z = 0) {   // a closing decoder: wait a moment
    if (i > 40) return NULL;
    usleep(20000);
  }
  HwDec *d = av_mallocz(sizeof *d);
  d->tb = time_base; d->tmp = av_packet_alloc();
  if (par->extradata_size && par->extradata[0] == 1) {   // avcC / hvcC (mp4, mkv): the engine wants Annex B
    if (av_bsf_alloc(av_bsf_get_by_name(h265 ? "hevc_mp4toannexb" : "h264_mp4toannexb"), &d->bsf) < 0) goto fail;
    avcodec_parameters_copy(d->bsf->par_in, par); d->bsf->time_base_in = time_base;
    if (av_bsf_init(d->bsf) < 0) goto fail;
  }
  if (!(d->dec = CreateVideoDecoder())) goto fail;
  static union { VideoStreamInfo v; char pad[1024]; } info; static union { VConfig v; char pad[4096]; } conf;   // padded: room for fields we don't know
  memset(&info, 0, sizeof info); memset(&conf, 0, sizeof conf);
  if (fps <= 0) fps = 25;
  info.v.eCodecFormat = h265 ? FMT_H265 : FMT_H264; info.v.nWidth = par->width; info.v.nHeight = par->height;
  info.v.nFrameRate = (int)(fps * 1000); info.v.nFrameDuration = (int)(1e6 / fps); info.v.bIsFramePackage = 1;
  if (!d->bsf && par->extradata_size) { info.v.pCodecSpecificData = (char *)par->extradata; info.v.nCodecSpecificDataLen = par->extradata_size; }
  conf.v.eOutputPixelFormat = PIX_YV12; conf.v.nDeInterlaceHoldingFrameBufferNum = 2; conf.v.nDisplayHoldingFrameBufferNum = 3;
  conf.v.nDecodeSmoothFrameBufferNum = 3; conf.v.memops = mem;
  if (InitializeVideoDecoder(d->dec, &info.v, &conf.v) != 0) goto fail;
  return d;
fail:
  hw_close(d);
  return NULL;
}

static int submit(HwDec *d, const AVPacket *p) {
  char *b0, *b1; int l0, l1;
  if (RequestVideoStreamBuffer(d->dec, p->size, &b0, &l0, &b1, &l1, 0) != 0 || l0 + l1 < p->size) return AVERROR(EAGAIN);
  if (p->size <= l0) memcpy(b0, p->data, p->size);
  else { memcpy(b0, p->data, l0); memcpy(b1, p->data + l0, p->size - l0); }
  VideoStreamDataInfo di; memset(&di, 0, sizeof di);
  di.pData = b0; di.nLength = p->size; di.bIsFirstPart = di.bIsLastPart = 1; di.bValid = 1;
  di.nPts = p->pts == AV_NOPTS_VALUE ? -1 : av_rescale_q(p->pts, d->tb, (AVRational){ 1, 1000000 });
  if (SubmitVideoStreamData(d->dec, &di, 0) != 0) { d->failed = 1; return AVERROR_EXTERNAL; }
  d->in++;
  return 0;
}

// AVERROR(EAGAIN): the engine is full and pkt was not taken (take pictures, then send it again). 0: taken.
static int hw_send(HwDec *d, AVPacket *pkt) {
  if (d->failed) return AVERROR_EXTERNAL;
  if (!d->bsf) return submit(d, pkt);
  if (d->tmp->size) {   // the filtered packet that didn't fit last time goes first
    int r = submit(d, d->tmp);
    if (r < 0) return r;
    av_packet_unref(d->tmp);
  }
  if (av_bsf_send_packet(d->bsf, pkt) < 0) { av_packet_unref(pkt); return 0; }   // one it can't use: skip it
  if (av_bsf_receive_packet(d->bsf, d->tmp) < 0) return 0;
  int r = submit(d, d->tmp);
  if (r == 0) av_packet_unref(d->tmp);
  return r == AVERROR(EAGAIN) ? 0 : r;   // EAGAIN here: kept in tmp for next time
}

// YV12 from the engine (Y, then V, then U; chroma rows half the luma stride) into a yuv420p frame, cropped.
static int copy_picture(VideoPicture *p, AVFrame *f) {
  int bw = p->nWidth, bh = p->nHeight, stride = p->nLineStride > 0 ? p->nLineStride : bw;
  int x0 = p->nLeftOffset, y0 = p->nTopOffset, x1 = p->nRightOffset, y1 = p->nBottomOffset;
  if (x1 <= x0 || x1 > bw) { x0 = 0; x1 = bw; }
  if (y1 <= y0 || y1 > bh) { y0 = 0; y1 = bh; }
  int w = (x1 - x0) & ~1, h = (y1 - y0) & ~1;
  mem->flush_cache(p->pData0, stride * bh * 3 / 2);
  f->format = AV_PIX_FMT_YUV420P; f->width = w; f->height = h;
  if (av_frame_get_buffer(f, 32) < 0) return AVERROR(ENOMEM);
  const uint8_t *Y = (const uint8_t *)p->pData0, *V = Y + stride * bh, *U = V + (stride / 2) * (bh / 2);
  for (int y = 0; y < h; y++) memcpy(f->data[0] + y * f->linesize[0], Y + (y0 + y) * stride + x0, w);
  for (int y = 0; y < h / 2; y++) {
    memcpy(f->data[1] + y * f->linesize[1], U + (y0 / 2 + y) * (stride / 2) + x0 / 2, w / 2);
    memcpy(f->data[2] + y * f->linesize[2], V + (y0 / 2 + y) * (stride / 2) + x0 / 2, w / 2);
  }
  f->pts = p->nPts;
  return 0;
}

// 1: a picture in f; 0: none yet; <0: the engine failed.
static int hw_receive(HwDec *d, AVFrame *f) {
  if (d->in > 120 && d->out == 0) d->failed = 1;   // nothing out after many packets in: not a stream it can do
  if (d->failed) return AVERROR_EXTERNAL;
  for (int tries = 0; tries < 4; tries++) {
    VideoPicture *p = RequestPicture(d->dec, 0);
    if (p) { int r = copy_picture(p, f); ReturnPicture(d->dec, p); d->out++; return r < 0 ? r : 1; }
    int r = DecodeVideoStream(d->dec, 0, 0, 0, 0);
    if (r < 0 || r == R_RESCHANGE) { d->failed = 1; return AVERROR_EXTERNAL; }
    if (r == R_NOBS) return 0;
  }
  return 0;
}

// --- the FFmpeg decoder -----------------------------------------------------------------------------------------
typedef struct {
  HwDec *hw;
  AVCodecContext *sw;      // software decoder, once the engine has given up on this file
  AVCodecParameters *par;
  AVRational tb;
  double fps;
  AVPacket *pkt;
  int eof, sw_drain;
} CedarCtx;

static int start_hw(AVCodecContext *avctx) {
  CedarCtx *c = avctx->priv_data;
  c->hw = hw_open(c->par, c->tb, c->fps);
  return c->hw ? 0 : AVERROR(ENOSYS);
}

static int start_sw(AVCodecContext *avctx) {
  CedarCtx *c = avctx->priv_data;
  hw_close(c->hw); c->hw = NULL;
  const AVCodec *dec = avcodec_find_decoder_by_name(avctx->codec_id == AV_CODEC_ID_HEVC ? "hevc" : "h264");
  if (!dec || !(c->sw = avcodec_alloc_context3(dec))) return AVERROR(ENOMEM);
  avcodec_parameters_to_context(c->sw, c->par);
  c->sw->pkt_timebase = avctx->pkt_timebase;
  c->sw->thread_count = 4;
  c->sw->flags2 |= AV_CODEC_FLAG2_FAST;
  c->sw->skip_loop_filter = AVDISCARD_NONKEY;
  av_log(avctx, AV_LOG_WARNING, "video engine gave up: decoding in software from here\n");
  return avcodec_open2(c->sw, dec, NULL);
}

static av_cold int cedar_init(AVCodecContext *avctx) {
  CedarCtx *c = avctx->priv_data;
  c->pkt = av_packet_alloc();
  c->par = avcodec_parameters_alloc();
  if (!c->pkt || !c->par) return AVERROR(ENOMEM);
  avcodec_parameters_from_context(c->par, avctx);
  c->tb = avctx->pkt_timebase.num > 0 ? avctx->pkt_timebase : (AVRational){ 1, 1000000 };
  c->fps = avctx->framerate.num > 0 ? av_q2d(avctx->framerate) : 0;
  avctx->pix_fmt = AV_PIX_FMT_YUV420P;
  int r = start_hw(avctx);
  if (r < 0) av_log(avctx, AV_LOG_INFO, "video engine not available for this stream\n");
  return r;   // failing here lets mpv use the normal decoder
}

static int sw_receive(AVCodecContext *avctx, AVFrame *frame) {
  CedarCtx *c = avctx->priv_data;
  for (;;) {
    int r = avcodec_receive_frame(c->sw, frame);
    if (r != AVERROR(EAGAIN)) return r;
    if (c->eof) {
      if (c->sw_drain) return AVERROR_EOF;
      c->sw_drain = 1; avcodec_send_packet(c->sw, NULL); continue;
    }
    if (!c->pkt->size) {
      r = ff_decode_get_packet(avctx, c->pkt);
      if (r == AVERROR_EOF) { c->eof = 1; continue; }
      if (r < 0) return r;
    }
    r = avcodec_send_packet(c->sw, c->pkt);
    if (r != AVERROR(EAGAIN)) av_packet_unref(c->pkt);
  }
}

static int cedar_receive_frame(AVCodecContext *avctx, AVFrame *frame) {
  CedarCtx *c = avctx->priv_data;
  if (c->sw) return sw_receive(avctx, frame);
  for (int spins = 0; ; spins++) {
    int r = hw_receive(c->hw, frame);
    if (r > 0) {
      frame->pts = frame->pts < 0 ? AV_NOPTS_VALUE : av_rescale_q(frame->pts, (AVRational){ 1, 1000000 }, c->tb);
      frame->pkt_dts = AV_NOPTS_VALUE;
      frame->sample_aspect_ratio = avctx->sample_aspect_ratio;
      return 0;
    }
    if (r < 0 || spins > 2000) { av_frame_unref(frame); if ((r = start_sw(avctx)) < 0) return r; return sw_receive(avctx, frame); }
    if (c->eof) return AVERROR_EOF;   // the engine has no drain: the last few pictures may be lost
    if (!c->pkt->size) {
      r = ff_decode_get_packet(avctx, c->pkt);
      if (r == AVERROR_EOF) { c->eof = 1; continue; }
      if (r < 0) return r;   // EAGAIN: mpv sends more
    }
    r = hw_send(c->hw, c->pkt);
    if (r == AVERROR(EAGAIN)) { if (spins > 200) usleep(1000); continue; }   // full: take pictures first
    av_packet_unref(c->pkt);
    if (r < 0) { if ((r = start_sw(avctx)) < 0) return r; return sw_receive(avctx, frame); }
  }
}

static void cedar_flush(AVCodecContext *avctx) {   // after a seek: start over (the engine has no reset we trust)
  CedarCtx *c = avctx->priv_data;
  av_packet_unref(c->pkt);
  c->eof = c->sw_drain = 0;
  if (c->sw) { avcodec_flush_buffers(c->sw); return; }
  hw_close(c->hw); c->hw = NULL;
  if (start_hw(avctx) < 0) start_sw(avctx);
}

static av_cold int cedar_close(AVCodecContext *avctx) {
  CedarCtx *c = avctx->priv_data;
  hw_close(c->hw); c->hw = NULL;
  avcodec_free_context(&c->sw);
  av_packet_free(&c->pkt);
  avcodec_parameters_free(&c->par);
  return 0;
}

#define CEDAR_DEC(NAME, LONG, ID) \
  const FFCodec ff_##NAME##_cedar_decoder = { \
    .p.name = #NAME "_cedar", .p.long_name = NULL_IF_CONFIG_SMALL("Allwinner video engine " LONG " (TrimUI Brick)"), \
    .p.type = AVMEDIA_TYPE_VIDEO, .p.id = ID, .priv_data_size = sizeof(CedarCtx), \
    .init = cedar_init, FF_CODEC_RECEIVE_FRAME_CB(cedar_receive_frame), .flush = cedar_flush, .close = cedar_close, \
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING, .caps_internal = FF_CODEC_CAP_INIT_CLEANUP, \
  };
CEDAR_DEC(h264, "H.264", AV_CODEC_ID_H264)
CEDAR_DEC(hevc, "HEVC", AV_CODEC_ID_HEVC)
