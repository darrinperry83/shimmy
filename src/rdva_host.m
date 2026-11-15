// rdva_host.m — host half of the VAAPI -> VideoToolbox media-engine bridge.
//
// Called from syscalls.c case 0x2003 (the guest's libva backend issues syscall(0x2003, ...)).
// The guest reconstructs an Annex-B H.264 access unit (SPS + PPS + slice NALs) from ffmpeg's
// parsed VA parameter buffers, and the host decodes it with a hardware VTDecompressionSession.
//
// Second increment (real ffmpeg): a *persistent* session is kept, keyed by the SPS/PPS bytes.
// The guest feeds one access unit per vaEndPicture (decode order) and pulls one decoded NV12
// frame per vaSyncSurface (display order). VideoToolbox reorders B-frames internally (using the
// picture order count the guest passes as the sample PTS) and emits frames in display order, so
// the host buffers outputs in a small FIFO that the guest drains in the same display order —
// the same POC order ffmpeg uses, so the two line up.
//
// Zero-copy IOSurface -> guest dma-buf hand-back is a follow-up; every decoded frame is copied
// out of the CVPixelBuffer into guest memory.

#import <VideoToolbox/VideoToolbox.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdva_host.h"

// -----------------------------------------------------------------------------------
// Annex-B H.264 helpers: find SPS/PPS and convert start codes -> AVCC length prefixes.
// -----------------------------------------------------------------------------------

static const uint8_t *h264_find_sc(const uint8_t *p, const uint8_t *end) {
  while (p + 3 <= end) {
    if (p[0] == 0x00 && p[1] == 0x00) {
      if (p[2] == 0x01) return p;                                   /* 00 00 01 */
      if (p[2] == 0x00 && p + 4 <= end && p[3] == 0x01) return p;   /* 00 00 00 01 */
    }
    p++;
  }
  return NULL;
}

static int h264_next_nal(const uint8_t **cursor, const uint8_t *end,
                         const uint8_t **nal, size_t *nal_len, int *nal_type) {
  const uint8_t *p = *cursor;
  const uint8_t *sc = h264_find_sc(p, end);
  if (!sc) return 0;
  int sc_len = (sc[2] == 0x01) ? 3 : 4;
  const uint8_t *start = sc + sc_len;
  if (start > end) return 0;
  const uint8_t *next = h264_find_sc(start, end);
  const uint8_t *stop = next ? next : end;
  *nal = start;
  *nal_len = (size_t)(stop - start);
  *nal_type = (*nal_len > 0) ? (start[0] & 0x1f) : 0;
  *cursor = next ? next : end;
  return 1;
}

// -----------------------------------------------------------------------------------
// Persistent decode session + display-order output FIFO.
//
// Locking: the session state (g_sess/g_fmt/g_sps_pps/g_pts) is only ever touched by the guest
// thread; the output FIFO (g_out) is shared between the guest thread and VideoToolbox's output
// callback thread, so it is guarded by g_rdva_lock. The lock is NEVER held while calling
// VTDecompressionSessionDecodeFrame / WaitForAsynchronousFrames — the output callback needs the
// lock to push a frame, and WaitForAsynchronousFrames blocks until that callback finishes.
// -----------------------------------------------------------------------------------

#define RDVA_OUT_DEPTH 32

typedef struct {
  uint8_t *data; size_t len; int valid;
} rdva_out_frame;

static pthread_mutex_t g_rdva_lock = PTHREAD_MUTEX_INITIALIZER;
static VTDecompressionSessionRef g_sess = NULL;
static CMVideoFormatDescriptionRef g_fmt = NULL;
static uint8_t  *g_sps_pps = NULL;      /* SPS+PPS bytes the current session was built from */
static size_t    g_sps_pps_len = 0;
static int64_t   g_pts = 0;             /* monotonic decode-order timestamp for the next frame */

static rdva_out_frame g_out[RDVA_OUT_DEPTH];
static int g_out_count = 0;             /* frames currently queued */
static int g_out_head  = 0;

/* Host-side trace toggle (set RDVA_TRACE=1 on the shim, not RDENV_RDVA_TRACE). */
static int rdva_host_trace(void) { static int t = -1; if (t < 0) t = getenv("RDVA_TRACE") != NULL; return t; }

/* VideoToolbox output callback: copy the decoded NV12 frame out and queue it. */
static void rdva_output_cb(void *refcon, void *sourceFrameRefCon,
                           OSStatus status, VTDecodeInfoFlags infoFlags,
                           CVImageBufferRef buffer,
                           CMTime presentationTimeStamp, CMTime presentationDuration) {
  (void)refcon; (void)sourceFrameRefCon; (void)infoFlags;
  if (status != noErr || !buffer) return;

  if (rdva_host_trace())
    fprintf(stderr, "[rdva-host] output cb: status=%d pts=%lld\n", (int)status,
            (long long)presentationTimeStamp.value);

  CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  int w = (int)CVPixelBufferGetWidth(buffer);
  int h = (int)CVPixelBufferGetHeight(buffer);
  size_t need = (size_t)w * (size_t)h * 3 / 2;

  uint8_t *y  = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(buffer, 0);
  uint8_t *uv = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(buffer, 1);
  size_t y_stride  = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
  size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);

  uint8_t *copy = y && uv ? (uint8_t *)malloc(need) : NULL;
  if (copy) {
    for (int row = 0; row < h; row++)
      memcpy(copy + (size_t)row * (size_t)w, y + (size_t)row * y_stride, (size_t)w);
    uint8_t *dst_uv = copy + (size_t)w * (size_t)h;
    int uvh = h / 2, uvw = w / 2;
    for (int row = 0; row < uvh; row++)
      memcpy(dst_uv + (size_t)row * (size_t)uvw * 2,
             uv + (size_t)row * uv_stride, (size_t)uvw * 2);
  }
  CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);

  if (!copy) return;

  pthread_mutex_lock(&g_rdva_lock);
  if (g_out_count < RDVA_OUT_DEPTH) {
    int idx = (g_out_head + g_out_count) % RDVA_OUT_DEPTH;
    if (g_out[idx].data) free(g_out[idx].data);   /* stale slot: drop */
    g_out[idx].data = copy; g_out[idx].len = need; g_out[idx].valid = 1;
    g_out_count++;
    copy = NULL;   /* ownership transferred */
  }
  pthread_mutex_unlock(&g_rdva_lock);
  free(copy);
}

/* Drop the cached session + FIFO (guest vaTerminate / SPS change). */
static void rdva_reset(void) {
  pthread_mutex_lock(&g_rdva_lock);
  if (g_sess) { VTDecompressionSessionInvalidate(g_sess); CFRelease(g_sess); g_sess = NULL; }
  if (g_fmt)  { CFRelease(g_fmt); g_fmt = NULL; }
  free(g_sps_pps); g_sps_pps = NULL; g_sps_pps_len = 0;
  g_pts = 0;
  for (int i = 0; i < RDVA_OUT_DEPTH; i++) {
    free(g_out[i].data); g_out[i].data = NULL; g_out[i].len = 0; g_out[i].valid = 0;
  }
  g_out_count = 0; g_out_head = 0;
  pthread_mutex_unlock(&g_rdva_lock);
}

/* (Re)create the session from the current SPS/PPS if it changed. Guest-thread only. */
static long rdva_ensure_session(const uint8_t *sps, size_t sps_len,
                                const uint8_t *pps, size_t pps_len) {
  size_t key_len = sps_len + pps_len;
  int same = g_sess && g_sps_pps && g_sps_pps_len == key_len &&
             memcmp(g_sps_pps, sps, sps_len) == 0 &&
             memcmp(g_sps_pps + sps_len, pps, pps_len) == 0;

  if (same) return 0;

  if (g_sess) { VTDecompressionSessionInvalidate(g_sess); CFRelease(g_sess); g_sess = NULL; }
  if (g_fmt)  { CFRelease(g_fmt); g_fmt = NULL; }

  const uint8_t *param_ptrs[2] = { sps, pps };
  const size_t  param_sizes[2] = { sps_len, pps_len };
  CMVideoFormatDescriptionRef fmt = NULL;
  OSStatus os = CMVideoFormatDescriptionCreateFromH264ParameterSets(
      kCFAllocatorDefault, 2, param_ptrs, param_sizes, 4 /*NALUnitHeaderLength*/, &fmt);
  if (os != noErr || !fmt) {
    fprintf(stderr, "[rdva] CMVideoFormatDescriptionCreateFromH264ParameterSets: %d\n", (int)os);
    return -EINVAL;
  }

  CFMutableDictionaryRef dest = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  int pxfmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
  CFNumberRef px = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pxfmt);
  CFDictionarySetValue(dest, kCVPixelBufferPixelFormatTypeKey, px);
  CFDictionarySetValue(dest, kCVPixelBufferIOSurfacePropertiesKey,
                       CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

  CFMutableDictionaryRef decspec = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(decspec,
      kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, kCFBooleanTrue);

  VTDecompressionOutputCallbackRecord cb = { rdva_output_cb, NULL };
  VTDecompressionSessionRef session = NULL;
  os = VTDecompressionSessionCreate(kCFAllocatorDefault, fmt, decspec, dest, &cb, &session);
  CFRelease(px); CFRelease(dest); CFRelease(decspec);
  if (os != noErr || !session) {
    fprintf(stderr, "[rdva] VTDecompressionSessionCreate: %d\n", (int)os);
    CFRelease(fmt);
    return -EIO;
  }

  uint8_t *key = (uint8_t *)malloc(key_len ? key_len : 1);
  if (!key) {
    VTDecompressionSessionInvalidate(session); CFRelease(session); CFRelease(fmt);
    return -ENOMEM;
  }
  memcpy(key, sps, sps_len);
  memcpy(key + sps_len, pps, pps_len);

  free(g_sps_pps);
  g_sps_pps = key; g_sps_pps_len = key_len;
  g_fmt = fmt; g_sess = session;
  return 0;
}

/* Feed one Annex-B access unit (SPS+PPS+slice) and drain all emitted outputs into the FIFO. */
static long rdva_feed(const uint8_t *bits, size_t bits_len, int64_t poc) {
  if (!bits || !bits_len) return -EINVAL;

  const uint8_t *sps = NULL, *pps = NULL;
  size_t sps_len = 0, pps_len = 0;
  {
    const uint8_t *cursor = bits;
    const uint8_t *end = bits + bits_len;
    const uint8_t *nal; size_t nal_len; int type;
    while (h264_next_nal(&cursor, end, &nal, &nal_len, &type)) {
      if (type == 7 && !sps) { sps = nal; sps_len = nal_len; }
      else if (type == 8 && !pps) { pps = nal; pps_len = nal_len; }
    }
  }
  if (!sps || !pps) {
    fprintf(stderr, "[rdva] no SPS/PPS in bitstream\n");
    return -EINVAL;
  }

  long rc = rdva_ensure_session(sps, sps_len, pps, pps_len);
  if (rc != 0) return rc;

  if (rdva_host_trace()) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < sps_len; i++) { h ^= sps[i]; h *= 1099511628211ULL; }
    for (size_t i = 0; i < pps_len; i++) { h ^= pps[i]; h *= 1099511628211ULL; }
    fprintf(stderr, "[rdva-host] sps_pps key=%016llx (sps=%zu pps=%zu)\n",
            (unsigned long long)h, sps_len, pps_len);
  }

  /* Convert the Annex-B access unit to AVCC (length-prefixed) for the sample buffer. */
  size_t avcc_cap = bits_len + 64;
  uint8_t *avcc = (uint8_t *)malloc(avcc_cap);
  if (!avcc) return -ENOMEM;
  size_t avcc_len = 0;
  {
    const uint8_t *cursor = bits;
    const uint8_t *end = bits + bits_len;
    const uint8_t *nal; size_t nal_len; int type;
    while (h264_next_nal(&cursor, end, &nal, &nal_len, &type)) {
      if (avcc_len + 4 + nal_len > avcc_cap) { free(avcc); return -EINVAL; }
      avcc[avcc_len + 0] = (uint8_t)(nal_len >> 24);
      avcc[avcc_len + 1] = (uint8_t)(nal_len >> 16);
      avcc[avcc_len + 2] = (uint8_t)(nal_len >> 8);
      avcc[avcc_len + 3] = (uint8_t)(nal_len);
      memcpy(avcc + avcc_len + 4, nal, nal_len);
      avcc_len += 4 + nal_len;
    }
  }

  CMBlockBufferRef block = NULL;
  OSStatus os = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, avcc, avcc_len,
      kCFAllocatorNull, NULL, 0, avcc_len, 0, &block);
  CMSampleBufferRef sample = NULL;
  if (os == noErr && block) {
    /* DTS = decode order (monotonic), PTS = POC (display order) so VideoToolbox reorders
     * B-frames correctly and emits in the same order ffmpeg reads the surfaces back. */
    CMSampleTimingInfo timing = {0};
    timing.decodeTimeStamp = CMTimeMake(g_pts++, 1);
    timing.presentationTimeStamp = CMTimeMake(poc, 1);
    timing.duration = kCMTimeInvalid;
    os = CMSampleBufferCreateReady(kCFAllocatorDefault, block, g_fmt, 1, 1, &timing,
                                   0, NULL, &sample);
  }

  rc = -EIO;
  if (os == noErr && sample) {
    VTDecodeInfoFlags infoOut = 0;
    os = VTDecompressionSessionDecodeFrame(g_sess, sample, 0, NULL, &infoOut);
    rc = (os == noErr) ? 0 : -EIO;
    if (rdva_host_trace())
      fprintf(stderr, "[rdva-host] feed: poc=%lld dts=%lld os=%d\n",
              (long long)poc, (long long)(g_pts - 1), (int)os);
    if (os != noErr) fprintf(stderr, "[rdva] VTDecompressionSessionDecodeFrame: %d\n", (int)os);
  } else {
    fprintf(stderr, "[rdva] sample-buffer build failed: %d\n", (int)os);
  }

  if (sample) CFRelease(sample);
  if (block)  CFRelease(block);
  free(avcc);

  /* Drain every output the decoder has produced so far into the FIFO (no lock held). */
  VTDecompressionSessionWaitForAsynchronousFrames(g_sess);

  return rc;
}

/* Pop the oldest decoded NV12 frame from the FIFO and copy it into the guest buffer. */
static long rdva_get_frame(uint8_t *out, size_t out_cap, uint64_t *out_written) {
  if (!out || !out_written) return -EFAULT;
  *out_written = 0;

  long rc = -EAGAIN;
  if (g_sess) {
    VTDecompressionSessionWaitForAsynchronousFrames(g_sess);
    pthread_mutex_lock(&g_rdva_lock);
    int empty = (g_out_count == 0);
    pthread_mutex_unlock(&g_rdva_lock);
    if (empty) {
      /* Force out any frame still sitting in the decoder's reorder buffer. */
      VTDecompressionSessionFinishDelayedFrames(g_sess);
      VTDecompressionSessionWaitForAsynchronousFrames(g_sess);
    }
  }

  pthread_mutex_lock(&g_rdva_lock);
  if (g_out_count > 0) {
    rdva_out_frame *f = &g_out[g_out_head];
    if (f->valid && f->data && f->len) {
      size_t n = f->len <= out_cap ? f->len : out_cap;
      memcpy(out, f->data, n);
      *out_written = n;
      rc = 0;
    }
    free(f->data); f->data = NULL; f->len = 0; f->valid = 0;
    g_out_head = (g_out_head + 1) % RDVA_OUT_DEPTH;
    g_out_count--;
  }
  if (rdva_host_trace())
    fprintf(stderr, "[rdva-host] get_frame: rc=%ld fifo=%d\n", rc, g_out_count);
  pthread_mutex_unlock(&g_rdva_lock);
  return rc;
}

// -----------------------------------------------------------------------------------
// syscall 0x2003 entry point.
// -----------------------------------------------------------------------------------
long rdva_bridge(uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3,
                 uint64_t a4, uint64_t a5) {
  switch (op) {
  case RDVA_OP_QUERY:
    return 1;  // bridge present, version 1

  case RDVA_OP_DECODE: {   // feed one access unit (decode order)
    const uint8_t *bits = (const uint8_t *)a1;
    size_t bits_len = (size_t)a2;
    int64_t poc = (int64_t)a3;   // picture order count (display order), for B-frame reordering
    return rdva_feed(bits, bits_len, poc);
  }

  case RDVA_OP_GET_FRAME: { // pull the next display-order frame's NV12
    uint8_t *out = (uint8_t *)a1;
    size_t out_cap = (size_t)a2;
    uint64_t *written = (uint64_t *)a3;
    return rdva_get_frame(out, out_cap, written);
  }

  case RDVA_OP_RESET:
    rdva_reset();
    return 0;

  default:
    return -ENOSYS;
  }
}
