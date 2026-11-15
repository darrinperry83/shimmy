/*
 * rdva_drv_video.c — a libva backend that routes H.264 decode to the shim's VideoToolbox
 * bridge (syscall 0x2003), for the rdmetal Linux-on-macOS guest.
 *
 * This is the guest half of the media-engine bridge. libva loads this as <name>_drv_video.so
 * (set LIBVA_DRIVER_NAME=rdva). It advertises H.264 VLD decode; on vaSyncSurface it marshals
 * a reconstructed Annex-B bitstream into the private syscall(0x2003, RDVA_OP_DECODE, ...) and
 * the shim (rdva_host.m) decodes it with a hardware VTDecompressionSession, copying NV12 back
 * into the guest buffer.
 *
 * REAL ffmpeg plumbing (the second increment): ffmpeg's vaapi decode does NOT hand over a
 * packed Annex-B file the way the first-cut probe did. It hands over *parsed* parameter
 * buffers — VAPictureParameterBufferH264 (+ VAIQMatrixBufferH264) and VASliceParameterBufferH264
 * + a raw VASliceDataBufferType per slice. VideoToolbox, however, needs the SPS/PPS as raw
 * NAL units to build its CMVideoFormatDescription. So this backend *reconstructs* the SPS and
 * PPS NALs from VAPictureParameterBufferH264 (profile/level are derived; scaling lists are
 * assumed flat) and prepends them to the slice NAL(s), producing a normal Annex-B access unit
 * for the host. The slice NAL itself is already in its original bitstream form inside
 * VASliceDataBufferType, so it needs only a start code.
 *
 * The full dma-buf/IOSurface zero-copy path (vaExportSurfaceHandle) is a follow-up.
 *
 * Cross-compiled for aarch64 against the Arch rootfs (see vaapi/build.sh). Not built by
 * build.sh's host CORE list — this is guest code.
 */

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_drmcommon.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define RDVA_SYSCALL     0x2003u
#define RDVA_OP_DECODE   1u
#define RDVA_OP_QUERY    2u
#define RDVA_OP_GET_FRAME 3u
#define RDVA_OP_RESET    4u

#define MAX_CONFIGS  16
#define MAX_CONTEXTS 16
#define MAX_SURFACES 64
#define MAX_BUFFERS  256

typedef struct { int used; VAProfile profile; VAEntrypoint entrypoint; } rdva_config;
typedef struct {
  int used; VAConfigID config_id; int width, height;
} rdva_context;
typedef struct {
  int used; int width, height;
  /* Accumulated Annex-B access unit for the picture currently being decoded. */
  unsigned char *pending; size_t pending_len, pending_cap;
  int have_sps;                     /* SPS+PPS have been prepended to pending */
  int poc;                          /* picture order count (display order) for the host */
  unsigned char *decoded; size_t decoded_len; int has_decoded;
} rdva_surface;
typedef struct {
  int used; VABufferType type; unsigned char *data; size_t size; int owned;
} rdva_buffer;

typedef struct {
  int fd;
  rdva_config configs[MAX_CONFIGS];
  rdva_context contexts[MAX_CONTEXTS];
  rdva_surface surfaces[MAX_SURFACES];
  rdva_buffer buffers[MAX_BUFFERS];
  unsigned next_config_id, next_context_id, next_surface_id, next_buffer_id;
} rdva_data;

static long rdva_syscall(long op, long a1, long a2, long a3, long a4, long a5) {
  return syscall(RDVA_SYSCALL, op, a1, a2, a3, a4, a5);
}

static int rdva_trace(void) { static int t = -1; if (t < 0) t = getenv("RDVA_TRACE") != NULL; return t; }

static rdva_data *data_of(VADriverContextP ctx) { return (rdva_data *)ctx->pDriverData; }

/* vaBeginPicture/vaRenderPicture/vaEndPicture are always issued back-to-back from the same
 * ffmpeg frame thread (ff_vaapi_decode_issue), but different frame threads interleave, so the
 * "current surface" is kept per-thread rather than in the shared context. */
static __thread VASurfaceID g_cur_surface = 0;

/* The driver is entered from ffmpeg's frame threads (decode) and its output thread (readback)
 * concurrently. The shared id counters must be bumped atomically so each caller gets a unique
 * id + a unique slot. */
static unsigned rdva_next(unsigned *counter) {
  return __atomic_fetch_add(counter, 1u, __ATOMIC_RELAXED);
}

static rdva_config *config_at(rdva_data *d, VAConfigID id) {
  if (id == 0 || id > MAX_CONFIGS) return NULL;
  rdva_config *c = &d->configs[id - 1];
  return c->used ? c : NULL;
}
static rdva_context *context_at(rdva_data *d, VAContextID id) {
  if (id == 0 || id > MAX_CONTEXTS) return NULL;
  rdva_context *c = &d->contexts[id - 1];
  return c->used ? c : NULL;
}
static rdva_surface *surface_at(rdva_data *d, VASurfaceID id) {
  if (id == 0 || id > MAX_SURFACES) return NULL;
  rdva_surface *s = &d->surfaces[id - 1];
  return s->used ? s : NULL;
}
static rdva_buffer *buffer_at(rdva_data *d, VABufferID id) {
  if (id == 0 || id > MAX_BUFFERS) return NULL;
  rdva_buffer *b = &d->buffers[id - 1];
  return b->used ? b : NULL;
}

static int h264_profile(VAProfile p) {
  return p == VAProfileH264ConstrainedBaseline || p == VAProfileH264Baseline ||
         p == VAProfileH264Main || p == VAProfileH264High;
}

/* ============================== bit-writer ============================== */

typedef struct {
  uint8_t *p; size_t cap; size_t bit; int err;
} bw_t;

static void bw_init(bw_t *b, uint8_t *buf, size_t cap) {
  b->p = buf; b->cap = cap; b->bit = 0; b->err = 0;
  memset(buf, 0, cap);
}

static void bw_bits(bw_t *b, uint32_t v, int n) {
  for (int i = n - 1; i >= 0 && !b->err; i--) {
    size_t byte = b->bit >> 3;
    if (byte >= b->cap) { b->err = 1; return; }
    if ((v >> i) & 1u) b->p[byte] |= (uint8_t)(1u << (7 - (b->bit & 7)));
    b->bit++;
  }
}

/* unsigned Exp-Golomb: codeNum = value + 1, written as (len) zeros + (len+1) bits */
static void bw_ue(bw_t *b, uint32_t v) {
  uint32_t code = v + 1;
  int len = 0; uint32_t t = code;
  while (t > 1) { t >>= 1; len++; }
  for (int i = 0; i < len; i++) bw_bits(b, 0, 1);
  bw_bits(b, code, len + 1);
}

/* signed Exp-Golomb */
static void bw_se(bw_t *b, int32_t v) {
  uint32_t code = (v <= 0) ? (uint32_t)(-2 * (int64_t)v) : (uint32_t)(2 * (int64_t)v - 1);
  bw_ue(b, code);
}

/* rbsp_stop_one_bit + zero padding to a byte boundary */
static void bw_trailing(bw_t *b) {
  bw_bits(b, 1, 1);
  while ((b->bit & 7) && !b->err) bw_bits(b, 0, 1);
}

/* ======================= H.264 SPS/PPS synthesis ======================= */

static int h264_profile_idc(const VAPictureParameterBufferH264 *pp) {
  if (pp->pic_fields.bits.transform_8x8_mode_flag) return 100; /* High */
  if (pp->pic_fields.bits.entropy_coding_mode_flag)  return 77;  /* Main  */
  return 66;                                                   /* Baseline */
}

static int h264_level_idc(int mbw, int mbh, int frame_mbs_only) {
  int max_fs = (frame_mbs_only ? 1 : 2) * mbw * mbh;
  /* {max_fs(MBs), level_idc}; pick the lowest level whose MaxFS is big enough. */
  static const int tbl[][2] = {
    { 99, 10 }, { 396, 11 }, { 396, 12 }, { 396, 13 },
    { 396, 20 }, { 792, 21 }, { 1620, 22 }, { 1620, 30 },
    { 3600, 31 }, { 5120, 32 }, { 8192, 40 }, { 8192, 41 },
    { 8704, 42 }, { 22080, 50 }, { 36864, 51 }, { 139264, 52 },
  };
  for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
    if (max_fs <= tbl[i][0]) return tbl[i][1];
  return 52;
}

/*
 * Reconstruct a sequence parameter set NAL (RBSP, no start code, no emulation-prevention
 * because these tiny bit patterns do not produce the 00 00 0x escape sequence — lengths are
 * small and the writer never emits two zero bytes followed by a 00..03 tail that would trip
 * the decoder, and the host re-derives start codes itself). Returns length via *out_len.
 */
static int build_sps(const VAPictureParameterBufferH264 *pp, uint8_t *out, size_t cap,
                     size_t *out_len) {
  bw_t b; bw_init(&b, out, cap);
  int poc_type = (int)pp->seq_fields.bits.pic_order_cnt_type;
  int mbw = (int)pp->picture_width_in_mbs_minus1 + 1;
  int mbh = (int)pp->picture_height_in_mbs_minus1 + 1;
  int frame_mbs_only = (int)pp->seq_fields.bits.frame_mbs_only_flag;

  bw_bits(&b, 0x67, 8);   /* NAL header: nal_ref_idc=3, nal_unit_type=7 (SPS) */
  bw_bits(&b, (uint32_t)h264_profile_idc(pp), 8);
  bw_bits(&b, 0, 8);                          /* 6 constraint flags + 2 reserved zeros */
  bw_bits(&b, (uint32_t)h264_level_idc(mbw, mbh, frame_mbs_only), 8);
  bw_ue(&b, 0);                               /* seq_parameter_set_id = 0 */
  bw_ue(&b, pp->seq_fields.bits.chroma_format_idc);
  /* separate_colour_plane_flag is only present when chroma_format_idc == 3 (not supported). */
  bw_ue(&b, pp->bit_depth_luma_minus8);
  bw_ue(&b, pp->bit_depth_chroma_minus8);
  bw_bits(&b, 0, 1);                          /* qpprime_y_zero_transform_bypass_flag */
  bw_bits(&b, 0, 1);                          /* seq_scaling_matrix_present_flag (flat) */
  bw_ue(&b, pp->seq_fields.bits.log2_max_frame_num_minus4);
  bw_ue(&b, (uint32_t)poc_type);
  if (poc_type == 0) {
    bw_ue(&b, pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
  } else if (poc_type == 1) {
    /* Best effort: we do not carry the POC cycle offsets in the VA struct. */
    bw_bits(&b, pp->seq_fields.bits.delta_pic_order_always_zero_flag, 1);
    bw_se(&b, 0);                             /* offset_for_non_ref_pic */
    bw_se(&b, 0);                             /* offset_for_top_to_bottom_field */
    bw_ue(&b, 0);                             /* num_ref_frames_in_pic_order_cnt_cycle = 0 */
  }
  bw_ue(&b, pp->num_ref_frames);              /* max_num_ref_frames */
  bw_bits(&b, pp->seq_fields.bits.gaps_in_frame_num_value_allowed_flag, 1);
  bw_ue(&b, pp->picture_width_in_mbs_minus1);
  bw_ue(&b, pp->picture_height_in_mbs_minus1);
  bw_bits(&b, (uint32_t)frame_mbs_only, 1);
  if (!frame_mbs_only)
    bw_bits(&b, pp->seq_fields.bits.mb_adaptive_frame_field_flag, 1);
  bw_bits(&b, pp->seq_fields.bits.direct_8x8_inference_flag, 1);
  bw_bits(&b, 0, 1);                          /* frame_cropping_flag = 0 */
  bw_bits(&b, 0, 1);                          /* vui_parameters_present_flag = 0 */
  bw_trailing(&b);

  *out_len = (b.bit + 7) / 8;
  return b.err ? -1 : 0;
}

static int build_pps(const VAPictureParameterBufferH264 *pp, uint8_t *out, size_t cap,
                     size_t *out_len) {
  bw_t b; bw_init(&b, out, cap);

  bw_bits(&b, 0x68, 8);   /* NAL header: nal_ref_idc=3, nal_unit_type=8 (PPS) */
  bw_ue(&b, 0);                               /* pic_parameter_set_id = 0 */
  bw_ue(&b, 0);                               /* seq_parameter_set_id = 0 */
  bw_bits(&b, pp->pic_fields.bits.entropy_coding_mode_flag, 1);
  bw_bits(&b, pp->pic_fields.bits.pic_order_present_flag, 1); /* bottom_field_pic_order_in_frame_present_flag */
  bw_ue(&b, 0);                               /* num_slice_groups_minus1 = 0 */
  bw_ue(&b, 0);                               /* num_ref_idx_l0_default_active_minus1 (x264-style default) */
  bw_ue(&b, 0);                               /* num_ref_idx_l1_default_active_minus1 */
  bw_bits(&b, pp->pic_fields.bits.weighted_pred_flag, 1);
  bw_bits(&b, pp->pic_fields.bits.weighted_bipred_idc, 2);
  bw_se(&b, pp->pic_init_qp_minus26);
  bw_se(&b, pp->pic_init_qs_minus26);
  bw_se(&b, pp->chroma_qp_index_offset);
  bw_bits(&b, pp->pic_fields.bits.deblocking_filter_control_present_flag, 1);
  bw_bits(&b, pp->pic_fields.bits.constrained_intra_pred_flag, 1);
  bw_bits(&b, pp->pic_fields.bits.redundant_pic_cnt_present_flag, 1);
  /* more_rbsp_data(): always emit the High-profile tail; it is harmless for Main/Baseline. */
  bw_bits(&b, pp->pic_fields.bits.transform_8x8_mode_flag, 1);
  bw_bits(&b, 0, 1);                          /* pic_scaling_matrix_present_flag = 0 (flat) */
  bw_se(&b, pp->second_chroma_qp_index_offset);
  bw_trailing(&b);

  *out_len = (b.bit + 7) / 8;
  return b.err ? -1 : 0;
}

/* ============================== driver entry points ============================== */

static VAStatus rdva_terminate(VADriverContextP ctx) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  for (int i = 0; i < MAX_SURFACES; i++) {
    free(d->surfaces[i].pending);
    free(d->surfaces[i].decoded);
    d->surfaces[i].pending = d->surfaces[i].decoded = NULL;
  }
  for (int i = 0; i < MAX_BUFFERS; i++) {
    if (d->buffers[i].owned) free(d->buffers[i].data);
    d->buffers[i].data = NULL;
  }
  free(d);
  ctx->pDriverData = NULL;
  rdva_syscall(RDVA_OP_RESET, 0, 0, 0, 0, 0);
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_query_config_profiles(VADriverContextP ctx,
                                           VAProfile *profile_list, int *num_profiles) {
  static const VAProfile profs[] = {
    VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High
  };
  int n = (int)(sizeof profs / sizeof profs[0]);
  (void)ctx;
  if (!num_profiles) return VA_STATUS_ERROR_INVALID_PARAMETER;
  if (profile_list) memcpy(profile_list, profs, sizeof profs);
  *num_profiles = n;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_query_config_entrypoints(VADriverContextP ctx, VAProfile profile,
                                              VAEntrypoint *entrypoint_list,
                                              int *num_entrypoints) {
  if (!h264_profile(profile)) {
    if (num_entrypoints) *num_entrypoints = 0;
    return VA_STATUS_SUCCESS;
  }
  if (!num_entrypoints) return VA_STATUS_ERROR_INVALID_PARAMETER;
  if (entrypoint_list) entrypoint_list[0] = VAEntrypointVLD;
  *num_entrypoints = 1;
  (void)ctx;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_get_config_attributes(VADriverContextP ctx, VAProfile profile,
                                           VAEntrypoint entrypoint,
                                           VAConfigAttrib *attrib_list, int num_attribs) {
  (void)ctx;
  if (profile != VAProfileNone && !h264_profile(profile))
    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
  if (entrypoint != VAEntrypointVLD) return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
  for (int i = 0; i < num_attribs; i++) {
    switch (attrib_list[i].type) {
    case VAConfigAttribRTFormat:      attrib_list[i].value = VA_RT_FORMAT_YUV420; break;
    case VAConfigAttribDecSliceMode:  attrib_list[i].value = VA_DEC_SLICE_MODE_NORMAL; break;
    default:                          attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED; break;
    }
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_create_config(VADriverContextP ctx, VAProfile profile,
                                   VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
                                   int num_attribs, VAConfigID *config_id) {
  (void)attrib_list; (void)num_attribs;
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  if (!h264_profile(profile)) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
  if (entrypoint != VAEntrypointVLD) return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
  unsigned idx = rdva_next(&d->next_config_id);
  if (idx >= MAX_CONFIGS) return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
  rdva_config *c = &d->configs[idx];
  c->used = 1; c->profile = profile; c->entrypoint = entrypoint;
  *config_id = idx + 1;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_destroy_config(VADriverContextP ctx, VAConfigID config_id) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  rdva_config *c = config_at(d, config_id);
  if (!c) return VA_STATUS_ERROR_INVALID_CONFIG;
  c->used = 0;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_query_config_attributes(VADriverContextP ctx, VAConfigID config_id,
                                             VAProfile *profile, VAEntrypoint *entrypoint,
                                             VAConfigAttrib *attrib_list, int *num_attribs) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  rdva_config *c = config_at(d, config_id);
  if (!c) return VA_STATUS_ERROR_INVALID_CONFIG;
  if (profile) *profile = c->profile;
  if (entrypoint) *entrypoint = c->entrypoint;
  if (num_attribs) {
    int n = 0;
    if (attrib_list) {
      attrib_list[n].type = VAConfigAttribRTFormat;
      attrib_list[n].value = VA_RT_FORMAT_YUV420; n++;
      attrib_list[n].type = VAConfigAttribDecSliceMode;
      attrib_list[n].value = VA_DEC_SLICE_MODE_NORMAL; n++;
    }
    *num_attribs = n;
  }
  return VA_STATUS_SUCCESS;
}

/* Allocate `num_surfaces` ids and record width/height. Shared by vaCreateSurfaces / 2. */
static VAStatus rdva_alloc_surfaces(rdva_data *d, int width, int height,
                                    int num_surfaces, VASurfaceID *surfaces) {
  if (num_surfaces <= 0 || !surfaces) return VA_STATUS_ERROR_INVALID_PARAMETER;
  for (int i = 0; i < num_surfaces; i++) {
    unsigned idx = rdva_next(&d->next_surface_id);
    if (idx >= MAX_SURFACES) return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    rdva_surface *s = &d->surfaces[idx];
    memset(s, 0, sizeof *s);
    s->used = 1; s->width = width; s->height = height;
    surfaces[i] = idx + 1;
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_create_surfaces(VADriverContextP ctx, int width, int height, int format,
                                     int num_surfaces, VASurfaceID *surfaces) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  if (!(format & VA_RT_FORMAT_YUV420)) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  return rdva_alloc_surfaces(d, width, height, num_surfaces, surfaces);
}

static VAStatus rdva_create_surfaces2(VADriverContextP ctx, unsigned int format,
                                      unsigned int width, unsigned int height,
                                      VASurfaceID *surfaces, unsigned int num_surfaces,
                                      VASurfaceAttrib *attrib_list, unsigned int num_attribs) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  if (!(format & VA_RT_FORMAT_YUV420)) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  for (unsigned int i = 0; i < num_attribs; i++) {
    if (attrib_list[i].type == VASurfaceAttribPixelFormat) {
      uint32_t fourcc = (uint32_t)attrib_list[i].value.value.i;
      if (fourcc != VA_FOURCC_NV12) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    } else if (attrib_list[i].type == VASurfaceAttribMemoryType) {
      uint32_t mt = (uint32_t)attrib_list[i].value.value.i;
      if (!(mt & VA_SURFACE_ATTRIB_MEM_TYPE_VA))
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    /* UsageHint/DRMFormatModifiers/etc are hints; accept them. */
  }
  return rdva_alloc_surfaces(d, (int)width, (int)height, (int)num_surfaces, surfaces);
}

static VAStatus rdva_destroy_surfaces(VADriverContextP ctx, VASurfaceID *surface_list,
                                      int num_surfaces) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  for (int i = 0; i < num_surfaces; i++) {
    rdva_surface *s = surface_at(d, surface_list[i]);
    if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;
    free(s->pending); free(s->decoded);
    s->pending = s->decoded = NULL;
    s->used = 0;
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_create_context(VADriverContextP ctx, VAConfigID config_id,
                                    int picture_width, int picture_height, int flag,
                                    VASurfaceID *render_targets, int num_render_targets,
                                    VAContextID *context) {
  (void)render_targets; (void)num_render_targets; (void)flag;
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  if (!config_at(d, config_id)) return VA_STATUS_ERROR_INVALID_CONFIG;
  unsigned idx = rdva_next(&d->next_context_id);
  if (idx >= MAX_CONTEXTS) return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
  rdva_context *c = &d->contexts[idx];
  memset(c, 0, sizeof *c);
  c->used = 1; c->config_id = config_id;
  c->width = picture_width; c->height = picture_height;
  *context = idx + 1;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_destroy_context(VADriverContextP ctx, VAContextID context) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  rdva_context *c = context_at(d, context);
  if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;
  c->used = 0;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_create_buffer(VADriverContextP ctx, VAContextID context, VABufferType type,
                                   unsigned int size, unsigned int num_elements, void *data,
                                   VABufferID *buf_id) {
  (void)context; (void)num_elements;
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  unsigned idx = rdva_next(&d->next_buffer_id);
  if (idx >= MAX_BUFFERS) return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
  rdva_buffer *b = &d->buffers[idx];
  memset(b, 0, sizeof *b);
  b->used = 1; b->type = type; b->size = size; b->owned = 1;
  b->data = malloc(size ? size : 1);
  if (!b->data) { b->used = 0; return VA_STATUS_ERROR_ALLOCATION_FAILED; }
  if (data && size) memcpy(b->data, data, size);
  else memset(b->data, 0, size);
  *buf_id = idx + 1;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_map_buffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  rdva_buffer *b = buffer_at(d, buf_id);
  if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;
  if (!pbuf) return VA_STATUS_ERROR_INVALID_PARAMETER;
  *pbuf = b->data;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_map_buffer2(VADriverContextP ctx, VABufferID buf_id, void **pbuf,
                                 uint32_t flags) {
  (void)flags;
  return rdva_map_buffer(ctx, buf_id, pbuf);
}

static VAStatus rdva_unmap_buffer(VADriverContextP ctx, VABufferID buf_id) {
  (void)ctx; (void)buf_id;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_destroy_buffer(VADriverContextP ctx, VABufferID buffer_id) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  rdva_buffer *b = buffer_at(d, buffer_id);
  if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;
  if (rdva_trace())
    fprintf(stderr, "[rdva] destroy_buffer id=%u type=%d\n", (unsigned)buffer_id, (int)b->type);
  if (b->owned) free(b->data);
  b->used = 0; b->data = NULL; b->size = 0; b->owned = 0;
  return VA_STATUS_SUCCESS;
}

/* append `n` bytes to a surface's pending Annex-B access unit */
static int surf_append(rdva_surface *s, const void *data, size_t n) {
  size_t want = s->pending_len + n;
  if (want > s->pending_cap) {
    size_t cap = s->pending_cap ? s->pending_cap : 1024;
    while (cap < want) cap *= 2;
    unsigned char *p = realloc(s->pending, cap);
    if (!p) return -1;
    s->pending = p; s->pending_cap = cap;
  }
  if (n) memcpy(s->pending + s->pending_len, data, n);
  s->pending_len = want;
  return 0;
}

static const uint8_t k_start_code[4] = { 0x00, 0x00, 0x00, 0x01 };

static VAStatus rdva_begin_picture(VADriverContextP ctx, VAContextID context,
                                   VASurfaceID render_target) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  rdva_context *c = context_at(d, context);
  if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;
  rdva_surface *s = surface_at(d, render_target);
  if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;
  g_cur_surface = render_target;
  s->pending_len = 0;
  s->have_sps = 0;
  s->has_decoded = 0;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_render_picture(VADriverContextP ctx, VAContextID context,
                                    VABufferID *buffers, int num_buffers) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  rdva_context *c = context_at(d, context);
  if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;
  rdva_surface *s = surface_at(d, g_cur_surface);
  if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;

  for (int i = 0; i < num_buffers; i++) {
    rdva_buffer *b = buffer_at(d, buffers[i]);
    if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;

    if (b->type == VAPictureParameterBufferType) {
      if (b->size < sizeof(VAPictureParameterBufferH264))
        return VA_STATUS_ERROR_INVALID_BUFFER;
      const VAPictureParameterBufferH264 *pp = (const VAPictureParameterBufferH264 *)b->data;
      uint8_t sps[256], pps[256];
      size_t sps_len = 0, pps_len = 0;
      if (build_sps(pp, sps, sizeof sps, &sps_len) != 0 ||
          build_pps(pp, pps, sizeof pps, &pps_len) != 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

      /* Start a fresh access unit: SPS + PPS, then the slice(s) follow. */
      s->pending_len = 0;
      if (surf_append(s, k_start_code, sizeof k_start_code) ||
          surf_append(s, sps, sps_len) ||
          surf_append(s, k_start_code, sizeof k_start_code) ||
          surf_append(s, pps, pps_len))
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
      s->have_sps = 1;
      /* ffmpeg stores the POC in "field units" (2 per frame for progressive), so halve it to
       * get the frame POC — the display order VideoToolbox uses as the sample PTS. */
      s->poc = (int)pp->CurrPic.TopFieldOrderCnt / 2;
      if (rdva_trace())
        fprintf(stderr, "[rdva] pic-param: %dx%d mbs poc=%d type=%d cabac=%d 8x8=%d -> sps=%zu pps=%zu\n",
                (int)pp->picture_width_in_mbs_minus1 + 1,
                (int)pp->picture_height_in_mbs_minus1 + 1,
                s->poc,
                (int)pp->seq_fields.bits.pic_order_cnt_type,
                (int)pp->pic_fields.bits.entropy_coding_mode_flag,
                (int)pp->pic_fields.bits.transform_8x8_mode_flag, sps_len, pps_len);

    } else if (b->type == VASliceDataBufferType) {
      /* The slice NAL is already in its original bitstream form (header + RBSP), so it
       * only needs a start code. VASliceParameterBufferType / VAIQMatrixBufferType carry
       * parsed data we have already accounted for. */
      if (surf_append(s, k_start_code, sizeof k_start_code) ||
          surf_append(s, b->data, b->size))
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
      if (rdva_trace())
        fprintf(stderr, "[rdva] slice-data: %zu bytes\n", b->size);
    }
    /* VAIQMatrixBufferType (flat scaling lists) and VASliceParameterBufferType are ignored. */
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_end_picture(VADriverContextP ctx, VAContextID context) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  rdva_context *c = context_at(d, context);
  if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;
  rdva_surface *s = surface_at(d, g_cur_surface);
  if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;

  /* Feed the reconstructed access unit to the host (decode order). The host decodes it into
   * its display-order FIFO; vaSyncSurface then drains that FIFO. */
  if (!s->pending || !s->pending_len) return VA_STATUS_SUCCESS;
  long rc = rdva_syscall(RDVA_OP_DECODE, (long)s->pending, (long)s->pending_len,
                         (long)s->poc, 0, 0);
  free(s->pending); s->pending = NULL; s->pending_len = s->pending_cap = 0;
  if (rc != 0) {
    fprintf(stderr, "[rdva] bridge feed failed: rc=%ld\n", rc);
    return VA_STATUS_ERROR_OPERATION_FAILED;
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_sync_surface(VADriverContextP ctx, VASurfaceID render_target) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  rdva_surface *s = surface_at(d, render_target);
  if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;

  size_t cap = (size_t)s->width * (size_t)s->height * 3 / 2;
  unsigned char *out = malloc(cap ? cap : 1);
  if (!out) return VA_STATUS_ERROR_ALLOCATION_FAILED;

  int64_t written = 0;
  long rc = rdva_syscall(RDVA_OP_GET_FRAME, (long)out, (long)cap, (long)&written, 0, 0);
  if (rc != 0 || written <= 0) {
    fprintf(stderr, "[rdva] bridge get-frame failed: rc=%ld written=%lld\n",
            rc, (long long)written);
    free(out);
    return VA_STATUS_ERROR_OPERATION_FAILED;
  }
  free(s->decoded);
  s->decoded = out;
  s->decoded_len = (size_t)written;
  s->has_decoded = 1;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_sync_surface2(VADriverContextP ctx, VASurfaceID surface,
                                   uint64_t timeout_ns) {
  (void)timeout_ns;
  return rdva_sync_surface(ctx, surface);
}

static VAStatus rdva_query_surface_status(VADriverContextP ctx, VASurfaceID render_target,
                                          VASurfaceStatus *status) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  if (!surface_at(d, render_target)) return VA_STATUS_ERROR_INVALID_SURFACE;
  if (status) *status = VASurfaceReady;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_query_image_formats(VADriverContextP ctx, VAImageFormat *format_list,
                                         int *num_formats) {
  (void)ctx;
  if (!num_formats) return VA_STATUS_ERROR_INVALID_PARAMETER;
  if (format_list && *num_formats < 1) return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
  if (format_list) {
    memset(&format_list[0], 0, sizeof format_list[0]);
    format_list[0].fourcc = VA_FOURCC_NV12;
    format_list[0].byte_order = VA_LSB_FIRST;
    format_list[0].bits_per_pixel = 12;
  }
  *num_formats = 1;
  return VA_STATUS_SUCCESS;
}

static void fill_nv12_image(VAImage *image, unsigned id, int width, int height) {
  memset(image, 0, sizeof *image);
  image->image_id = id;
  image->format.fourcc = VA_FOURCC_NV12;
  image->format.byte_order = VA_LSB_FIRST;
  image->format.bits_per_pixel = 12;
  image->width = (uint16_t)width;
  image->height = (uint16_t)height;
  image->data_size = (uint32_t)((size_t)width * (size_t)height * 3 / 2);
  image->num_planes = 2;
  image->pitches[0] = (uint32_t)width;
  image->pitches[1] = (uint32_t)width;
  image->offsets[0] = 0;
  image->offsets[1] = (uint32_t)((size_t)width * (size_t)height);
}

static VAStatus rdva_create_image(VADriverContextP ctx, VAImageFormat *format,
                                  int width, int height, VAImage *image) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  if (!format || format->fourcc != VA_FOURCC_NV12)
    return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
  unsigned idx = rdva_next(&d->next_buffer_id);
  if (idx >= MAX_BUFFERS) return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
  rdva_buffer *b = &d->buffers[idx];
  memset(b, 0, sizeof *b);
  b->used = 1; b->type = VAImageBufferType;
  b->size = (size_t)width * (size_t)height * 3 / 2;
  b->data = calloc(1, b->size);
  b->owned = 1;
  if (!b->data) { b->used = 0; return VA_STATUS_ERROR_ALLOCATION_FAILED; }
  unsigned id = idx + 1;
  fill_nv12_image(image, id, width, height);
  image->buf = id;
  if (rdva_trace())
    fprintf(stderr, "[rdva] create_image: id=%u buf=%u d=%p b=%p img=%p used=%d\n",
            (unsigned)image->image_id, (unsigned)image->buf, (void*)d,
            (void*)&d->buffers[idx], (void*)image, d->buffers[idx].used);
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_derive_image(VADriverContextP ctx, VASurfaceID surface, VAImage *image) {
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  rdva_surface *s = surface_at(d, surface);
  if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;
  if (!s->has_decoded) return VA_STATUS_ERROR_INVALID_SURFACE;
  unsigned idx = rdva_next(&d->next_buffer_id);
  if (idx >= MAX_BUFFERS) return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
  rdva_buffer *b = &d->buffers[idx];
  memset(b, 0, sizeof *b);
  b->used = 1; b->type = VAImageBufferType;
  b->data = s->decoded;   /* the surface owns the storage */
  b->size = s->decoded_len;
  b->owned = 0;
  unsigned id = idx + 1;
  fill_nv12_image(image, id, s->width, s->height);
  image->buf = id;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_destroy_image(VADriverContextP ctx, VAImageID image) {
  return rdva_destroy_buffer(ctx, image);
}

static VAStatus rdva_get_image(VADriverContextP ctx, VASurfaceID surface,
                               int x, int y, unsigned int width, unsigned int height,
                               VAImageID image) {
  (void)x; (void)y; (void)width; (void)height;
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  rdva_surface *s = surface_at(d, surface);
  if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;
  if (!s->has_decoded) return VA_STATUS_ERROR_INVALID_SURFACE;
  rdva_buffer *b = buffer_at(d, image);
  if (!b) {
    if (rdva_trace())
      fprintf(stderr, "[rdva] get_image: image=%u FAIL (next=%u used=%d d=%p b=%p)\n",
              (unsigned)image, d->next_buffer_id,
              (image > 0 && image <= MAX_BUFFERS) ? d->buffers[image - 1].used : -1,
              (void*)d, (void*)&d->buffers[image - 1]);
    return VA_STATUS_ERROR_INVALID_IMAGE;
  }
  size_t n = b->size < s->decoded_len ? b->size : s->decoded_len;
  memcpy(b->data, s->decoded, n);
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_put_image(VADriverContextP ctx, VASurfaceID surface, VAImageID image,
                               int src_x, int src_y, unsigned int src_width,
                               unsigned int src_height, int dest_x, int dest_y,
                               unsigned int dest_width, unsigned int dest_height) {
  (void)src_x; (void)src_y; (void)src_width; (void)src_height;
  (void)dest_x; (void)dest_y; (void)dest_width; (void)dest_height;
  rdva_data *d = data_of(ctx);
  if (!d) return VA_STATUS_ERROR_INVALID_DISPLAY;
  rdva_surface *s = surface_at(d, surface);
  if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;
  rdva_buffer *b = buffer_at(d, image);
  if (!b) return VA_STATUS_ERROR_INVALID_IMAGE;
  /* Upload the image bytes into the surface's backing store (decode targets normally hold
   * hardware-decoded NV12; for uploads we just retain the client's bytes). */
  unsigned char *copy = malloc(b->size ? b->size : 1);
  if (!copy) return VA_STATUS_ERROR_ALLOCATION_FAILED;
  memcpy(copy, b->data, b->size);
  free(s->decoded);
  s->decoded = copy;
  s->decoded_len = b->size;
  s->has_decoded = 1;
  return VA_STATUS_SUCCESS;
}

/* --- stubs: libva requires these vtable entries to be non-NULL at init time --- */

static VAStatus rdva_buffer_set_num_elements(VADriverContextP ctx, VABufferID buf_id,
                                             unsigned int num_elements) {
  (void)ctx; (void)buf_id; (void)num_elements;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_set_image_palette(VADriverContextP ctx, VAImageID image,
                                       unsigned char *palette) {
  (void)ctx; (void)image; (void)palette;
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus rdva_query_surface_error(VADriverContextP ctx, VASurfaceID render_target,
                                         VAStatus error_status, void **error_info) {
  (void)ctx; (void)render_target; (void)error_status;
  if (error_info) *error_info = NULL;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_put_surface(VADriverContextP ctx, VASurfaceID surface, void *draw,
                                 short srcx, short srcy, unsigned short srcw, unsigned short srch,
                                 short destx, short desty, unsigned short destw,
                                 unsigned short desth, VARectangle *cliprects,
                                 unsigned int number_cliprects, unsigned int flags) {
  (void)ctx; (void)surface; (void)draw; (void)srcx; (void)srcy; (void)srcw; (void)srch;
  (void)destx; (void)desty; (void)destw; (void)desth; (void)cliprects;
  (void)number_cliprects; (void)flags;
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus rdva_query_subpicture_formats(VADriverContextP ctx, VAImageFormat *format_list,
                                              unsigned int *flags, unsigned int *num_formats) {
  (void)ctx; (void)format_list; (void)flags;
  if (num_formats) *num_formats = 0;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_create_subpicture(VADriverContextP ctx, VAImageID image,
                                       VASubpictureID *subpicture) {
  (void)ctx; (void)image; (void)subpicture;
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus rdva_destroy_subpicture(VADriverContextP ctx, VASubpictureID subpicture) {
  (void)ctx; (void)subpicture;
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus rdva_set_subpicture_image(VADriverContextP ctx, VASubpictureID subpicture,
                                          VAImageID image) {
  (void)ctx; (void)subpicture; (void)image;
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus rdva_set_subpicture_chromakey(VADriverContextP ctx, VASubpictureID subpicture,
                                              unsigned int chromakey_min,
                                              unsigned int chromakey_max,
                                              unsigned int chromakey_mask) {
  (void)ctx; (void)subpicture; (void)chromakey_min; (void)chromakey_max; (void)chromakey_mask;
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus rdva_set_subpicture_global_alpha(VADriverContextP ctx, VASubpictureID subpicture,
                                                 float global_alpha) {
  (void)ctx; (void)subpicture; (void)global_alpha;
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus rdva_associate_subpicture(VADriverContextP ctx, VASubpictureID subpicture,
                                          VASurfaceID *target_surfaces, int num_surfaces,
                                          short src_x, short src_y, unsigned short src_width,
                                          unsigned short src_height, short dest_x, short dest_y,
                                          unsigned short dest_width, unsigned short dest_height,
                                          unsigned int flags) {
  (void)ctx; (void)subpicture; (void)target_surfaces; (void)num_surfaces; (void)src_x;
  (void)src_y; (void)src_width; (void)src_height; (void)dest_x; (void)dest_y;
  (void)dest_width; (void)dest_height; (void)flags;
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus rdva_deassociate_subpicture(VADriverContextP ctx, VASubpictureID subpicture,
                                            VASurfaceID *target_surfaces, int num_surfaces) {
  (void)ctx; (void)subpicture; (void)target_surfaces; (void)num_surfaces;
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus rdva_query_display_attributes(VADriverContextP ctx, VADisplayAttribute *attr_list,
                                              int *num_attributes) {
  (void)ctx; (void)attr_list;
  if (num_attributes) *num_attributes = 0;
  return VA_STATUS_SUCCESS;
}

static VAStatus rdva_get_display_attributes(VADriverContextP ctx, VADisplayAttribute *attr_list,
                                            int num_attributes) {
  (void)ctx; (void)attr_list; (void)num_attributes;
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus rdva_set_display_attributes(VADriverContextP ctx, VADisplayAttribute *attr_list,
                                            int num_attributes) {
  (void)ctx; (void)attr_list; (void)num_attributes;
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/*
 * vaQuerySurfaceAttributes — ffmpeg drives this hard: it queries the config's surface
 * attributes to (a) pick a usable software format and (b) bound the min/max dimensions, then
 * (c) passes the chosen pixel format + memory type back into vaCreateSurfaces.
 */
static VAStatus rdva_query_surface_attributes(VADriverContextP ctx, VAConfigID config,
                                              VASurfaceAttrib *attrib_list,
                                              unsigned int *num_attribs) {
  (void)ctx; (void)config;
  if (!num_attribs) return VA_STATUS_ERROR_INVALID_PARAMETER;

  const int total = 6;
  if (!attrib_list) {
    *num_attribs = (unsigned int)total;
    return VA_STATUS_SUCCESS;
  }

  int n = (*num_attribs < (unsigned int)total) ? (int)*num_attribs : total;
  static const VASurfaceAttrib base[6] = {
    { VASurfaceAttribPixelFormat, VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
      { VAGenericValueTypeInteger, { .i = (int32_t)VA_FOURCC_NV12 } } },
    { VASurfaceAttribMemoryType, VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
      { VAGenericValueTypeInteger, { .i = (int32_t)VA_SURFACE_ATTRIB_MEM_TYPE_VA } } },
    { VASurfaceAttribMinWidth, VA_SURFACE_ATTRIB_GETTABLE,
      { VAGenericValueTypeInteger, { .i = 16 } } },
    { VASurfaceAttribMinHeight, VA_SURFACE_ATTRIB_GETTABLE,
      { VAGenericValueTypeInteger, { .i = 16 } } },
    { VASurfaceAttribMaxWidth, VA_SURFACE_ATTRIB_GETTABLE,
      { VAGenericValueTypeInteger, { .i = 8192 } } },
    { VASurfaceAttribMaxHeight, VA_SURFACE_ATTRIB_GETTABLE,
      { VAGenericValueTypeInteger, { .i = 8192 } } },
  };
  memcpy(attrib_list, base, (size_t)n * sizeof(VASurfaceAttrib));
  *num_attribs = (unsigned int)n;
  return VA_STATUS_SUCCESS;
}

/* vaExportSurfaceHandle — dma-buf zero-copy hand-back is a follow-up. */
static VAStatus rdva_export_surface_handle(VADriverContextP ctx, VASurfaceID surface_id,
                                           uint32_t mem_type, uint32_t flags,
                                           void *descriptor) {
  (void)ctx; (void)surface_id; (void)mem_type; (void)flags; (void)descriptor;
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* ------------------------------ driver init ------------------------------ */

static VAStatus rdva_init(VADriverContextP ctx) {
  rdva_data *d = calloc(1, sizeof *d);
  if (!d) return VA_STATUS_ERROR_ALLOCATION_FAILED;
  d->fd = -1;
  if (ctx->drm_state) {
    struct drm_state *st = (struct drm_state *)ctx->drm_state;
    d->fd = st->fd;
  } else if (ctx->native_dpy) {
    struct drm_state *st = (struct drm_state *)ctx->native_dpy;
    d->fd = st->fd;
  }

  ctx->pDriverData = d;
  ctx->str_vendor = "rdmetal (Apple VideoToolbox)";
  ctx->version_major = 1;
  ctx->version_minor = 0;
  ctx->max_profiles = MAX_CONFIGS;
  ctx->max_entrypoints = 1;
  ctx->max_attributes = 2;
  ctx->max_image_formats = 1;
  ctx->max_subpic_formats = 4;
  ctx->max_display_attributes = 4;

  struct VADriverVTable *v = ctx->vtable;
  v->vaTerminate = rdva_terminate;
  v->vaQueryConfigProfiles = rdva_query_config_profiles;
  v->vaQueryConfigEntrypoints = rdva_query_config_entrypoints;
  v->vaGetConfigAttributes = rdva_get_config_attributes;
  v->vaQueryConfigAttributes = rdva_query_config_attributes;
  v->vaCreateConfig = rdva_create_config;
  v->vaDestroyConfig = rdva_destroy_config;
  v->vaCreateSurfaces = rdva_create_surfaces;
  v->vaDestroySurfaces = rdva_destroy_surfaces;
  v->vaCreateContext = rdva_create_context;
  v->vaDestroyContext = rdva_destroy_context;
  v->vaCreateBuffer = rdva_create_buffer;
  v->vaBufferSetNumElements = rdva_buffer_set_num_elements;
  v->vaMapBuffer = rdva_map_buffer;
  v->vaUnmapBuffer = rdva_unmap_buffer;
  v->vaDestroyBuffer = rdva_destroy_buffer;
  v->vaBeginPicture = rdva_begin_picture;
  v->vaRenderPicture = rdva_render_picture;
  v->vaEndPicture = rdva_end_picture;
  v->vaSyncSurface = rdva_sync_surface;
  v->vaQuerySurfaceStatus = rdva_query_surface_status;
  v->vaQuerySurfaceError = rdva_query_surface_error;
  v->vaPutSurface = rdva_put_surface;
  v->vaQueryImageFormats = rdva_query_image_formats;
  v->vaCreateImage = rdva_create_image;
  v->vaDeriveImage = rdva_derive_image;
  v->vaDestroyImage = rdva_destroy_image;
  v->vaSetImagePalette = rdva_set_image_palette;
  v->vaGetImage = rdva_get_image;
  v->vaPutImage = rdva_put_image;
  v->vaQuerySubpictureFormats = rdva_query_subpicture_formats;
  v->vaCreateSubpicture = rdva_create_subpicture;
  v->vaDestroySubpicture = rdva_destroy_subpicture;
  v->vaSetSubpictureImage = rdva_set_subpicture_image;
  v->vaSetSubpictureChromakey = rdva_set_subpicture_chromakey;
  v->vaSetSubpictureGlobalAlpha = rdva_set_subpicture_global_alpha;
  v->vaAssociateSubpicture = rdva_associate_subpicture;
  v->vaDeassociateSubpicture = rdva_deassociate_subpicture;
  v->vaQueryDisplayAttributes = rdva_query_display_attributes;
  v->vaGetDisplayAttributes = rdva_get_display_attributes;
  v->vaSetDisplayAttributes = rdva_set_display_attributes;
  /* --- the surface plumbing ffmpeg actually uses (second increment) --- */
  v->vaCreateSurfaces2 = rdva_create_surfaces2;
  v->vaQuerySurfaceAttributes = rdva_query_surface_attributes;
  v->vaSyncSurface2 = rdva_sync_surface2;
  v->vaMapBuffer2 = rdva_map_buffer2;
  v->vaExportSurfaceHandle = rdva_export_surface_handle;

  fprintf(stderr, "[rdva] driver init OK (drm fd=%d)\n", d->fd);
  return VA_STATUS_SUCCESS;
}

#define RDVA_EXPORT __attribute__((visibility("default")))

RDVA_EXPORT VAStatus __vaDriverInit_1_24(VADriverContextP ctx) { return rdva_init(ctx); }
RDVA_EXPORT VAStatus __vaDriverInit_1_0(VADriverContextP ctx)  { return rdva_init(ctx); }
RDVA_EXPORT VAStatus __vaDriverInit_0_32(VADriverContextP ctx) { return rdva_init(ctx); }
