// glxdecode.c — GLX indirect-rendering wire decoder -> glbridge (GL on Metal).
//
// Every opcode number and payload layout below is taken from the GLX 1.4 protocol
// specification (the numbers match <GL/glxproto.h>'s X_GLrop_* / X_GLsop_* defines).
// Traps worth remembering, all of which produce garbage geometry rather than an error:
//   * a rendering command's `length` counts the 4-byte {length,opcode} header too, so the
//     walk advances by `length`, not by `length + 4`;
//   * glFrustum / glOrtho / glClearDepth / glRotated&co take FLOAT64 on the wire even though
//     the app called the "f" entry point — glxgears' glFrustum is a 52-byte command;
//   * the "v" form is the only one on the wire: glVertex3f is encoded as Vertex3fv, glNormal3f
//     as Normal3fv, and so on.  There is no scalar form to decode;
//   * doubles sit at 4-byte alignment inside a command, so they must be read with memcpy;
//   * glNewList/glEndList/glGenLists are GLX *single* requests, not rendering commands: they
//     arrive out of band but in socket order, so the list body still brackets correctly.
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdarg.h>

#include "glxdecode.h"
#include "glbridge.h"

// ---------------------------------------------------------------------------
// GL rendering opcodes (X_GLrop_*)
// ---------------------------------------------------------------------------
#define ROP_CallList        1
#define ROP_CallLists       2
#define ROP_ListBase        3
#define ROP_Begin           4
#define ROP_Color3bv        6
#define ROP_Color3dv        7
#define ROP_Color3fv        8
#define ROP_Color3iv        9
#define ROP_Color3sv       10
#define ROP_Color3ubv      11
#define ROP_Color4bv       14
#define ROP_Color4dv       15
#define ROP_Color4fv       16
#define ROP_Color4iv       17
#define ROP_Color4sv       18
#define ROP_Color4ubv      19
#define ROP_EdgeFlagv      22
#define ROP_End            23
#define ROP_Normal3bv      28
#define ROP_Normal3dv      29
#define ROP_Normal3fv      30
#define ROP_Normal3iv      31
#define ROP_Normal3sv      32
#define ROP_TexCoord1fv    50
#define ROP_TexCoord2dv    53
#define ROP_TexCoord2fv    54
#define ROP_TexCoord3fv    58
#define ROP_TexCoord4fv    62
#define ROP_Vertex2dv      65
#define ROP_Vertex2fv      66
#define ROP_Vertex2iv      67
#define ROP_Vertex2sv      68
#define ROP_Vertex3dv      69
#define ROP_Vertex3fv      70
#define ROP_Vertex3iv      71
#define ROP_Vertex3sv      72
#define ROP_Vertex4dv      73
#define ROP_Vertex4fv      74
#define ROP_ColorMaterial  78
#define ROP_CullFace       79
#define ROP_FrontFace      84
#define ROP_Hint           85
#define ROP_Lightf         86
#define ROP_Lightfv        87
#define ROP_Lighti         88
#define ROP_Lightiv        89
#define ROP_LightModelf    90
#define ROP_LightModelfv   91
#define ROP_LightModeli    92
#define ROP_LightModeliv   93
#define ROP_LineWidth      95
#define ROP_Materialf      96
#define ROP_Materialfv     97
#define ROP_Materiali      98
#define ROP_Materialiv     99
#define ROP_PointSize     100
#define ROP_PolygonMode   101
#define ROP_Scissor       103
#define ROP_ShadeModel    104
#define ROP_TexParameterf 105
#define ROP_TexParameteri 107
#define ROP_DrawBuffer    126
#define ROP_Clear         127
#define ROP_ClearAccum    128
#define ROP_ClearIndex    129
#define ROP_ClearColor    130
#define ROP_ClearStencil  131
#define ROP_ClearDepth    132
#define ROP_StencilMask   133
#define ROP_ColorMask     134
#define ROP_DepthMask     135
#define ROP_Disable       138
#define ROP_Enable        139
#define ROP_PopAttrib     141
#define ROP_PushAttrib    142
#define ROP_AlphaFunc     159
#define ROP_BlendFunc     160
#define ROP_LogicOp       161
#define ROP_StencilFunc   162
#define ROP_StencilOp     163
#define ROP_DepthFunc     164
#define ROP_ReadBuffer    171
#define ROP_DepthRange    174
#define ROP_Frustum       175
#define ROP_LoadIdentity  176
#define ROP_LoadMatrixf   177
#define ROP_LoadMatrixd   178
#define ROP_MatrixMode    179
#define ROP_MultMatrixf   180
#define ROP_MultMatrixd   181
#define ROP_Ortho         182
#define ROP_PopMatrix     183
#define ROP_PushMatrix    184
#define ROP_Rotated       185
#define ROP_Rotatef       186
#define ROP_Scaled        187
#define ROP_Scalef        188
#define ROP_Translated    189
#define ROP_Translatef    190
#define ROP_Viewport      191
#define ROP_PolygonOffset 192
#define ROP_BindTexture  4117

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static int      g_lsb = 1;              // connection byte order (LSBFirst by default)
static int      g_ready;                // glb_init succeeded
static int      g_fb_w, g_fb_h;
static int      g_content;              // saw a real drawing command
static uint64_t g_ncmds;
static int      g_verbose = -1;

// RenderLarge reassembly
static uint8_t* g_large;
static size_t   g_large_len, g_large_cap;
static uint16_t g_large_next;           // the requestNumber we expect next (1-based)
static uint16_t g_large_total;

static void gdbg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void gdbg(const char* fmt, ...) {
  if (g_verbose < 0) {
    const char* e = getenv("RDVERBOSE");
    if (!e || !*e || *e == '0') e = getenv("X11SRV_DEBUG");
    g_verbose = (e && *e && *e != '0');
  }
  if (!g_verbose) return;
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[glx] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
  va_end(ap);
}

// ---------------------------------------------------------------------------
// byte-order-aware unaligned readers
// ---------------------------------------------------------------------------
#if defined(__BIG_ENDIAN__) && __BIG_ENDIAN__
#  define HOST_LSB 0
#else
#  define HOST_LSB 1
#endif
static inline int need_swap(void) { return g_lsb != HOST_LSB; }

static inline uint16_t r16(const uint8_t* p) {
  uint16_t v; memcpy(&v, p, 2);
  return need_swap() ? __builtin_bswap16(v) : v;
}
static inline uint32_t r32(const uint8_t* p) {
  uint32_t v; memcpy(&v, p, 4);
  return need_swap() ? __builtin_bswap32(v) : v;
}
static inline int32_t ri32(const uint8_t* p) { return (int32_t)r32(p); }
static inline float rf32(const uint8_t* p) {
  uint32_t v = r32(p); float f; memcpy(&f, &v, 4); return f;
}
static inline double rf64(const uint8_t* p) {
  uint64_t v; memcpy(&v, p, 8);
  if (need_swap()) v = __builtin_bswap64(v);
  double d; memcpy(&d, &v, 8); return d;
}

// Read up to `max` FLOAT32s from `p` (`n` available), zero-filling the rest.
static int read_floats(const uint8_t* p, size_t avail, int max, float* out) {
  int n = (int)(avail / 4);
  if (n > max) n = max;
  for (int i = 0; i < max; i++) out[i] = (i < n) ? rf32(p + i * 4) : 0.0f;
  return n;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
void glxd_set_byte_order(int lsb_first) { g_lsb = lsb_first ? 1 : 0; }

int glxd_ensure(int w, int h) {
  if (w <= 0 || h <= 0) return -22;
  if (!g_ready) {
    int rc = glb_init(NULL);
    if (rc != 0) { fprintf(stderr, "[glx] glb_init failed: %d\n", rc); return rc; }
    g_ready = 1;
    g_fb_w = g_fb_h = 0;
  }
  if (w != g_fb_w || h != g_fb_h) {
    int rc = glb_resize(w, h);
    if (rc != 0) { fprintf(stderr, "[glx] glb_resize(%d,%d) failed: %d\n", w, h, rc); return rc; }
    g_fb_w = w; g_fb_h = h;
    gdbg("framebuffer %dx%d", w, h);
  }
  return 0;
}

int      glxd_has_content(void)   { return g_content; }
uint64_t glxd_command_count(void) { return g_ncmds; }

// ---------------------------------------------------------------------------
// RDANGLE probe: the animation angle of ONE gear, sampled once per PRESENTED frame.
//
// This is the honest measure of smoothness. glxgears derives its rotation from the wall clock
// at draw time, so what the eye integrates is the angle carried by the frames that actually
// reach the display — not the rate at which frames are drawn. If the presenter throttles, the
// client keeps drawing frames whose angles barely differ and then one of them is shown after a
// long gap: bursts of ~0 advance punctuated by lurches.
// ---------------------------------------------------------------------------
#define ANGLE_PROBE_SLOT 3          // 4th glRotatef of the frame: one of the gears
static int   g_frame_rots;
static float g_frame_angle;

static int angle_probe_on(void) {
  static int v = -1;
  if (v < 0) { const char* e = getenv("RDANGLE"); v = (e && *e && *e != '0'); }
  return v;
}

// ---------------------------------------------------------------------------
// one rendering command
//   `a`/`n` are the payload: the bytes AFTER the {length,opcode} header.
// ---------------------------------------------------------------------------
static void unknown_rop(uint32_t op, size_t n) {
  static uint8_t seen[512];
  if (op < sizeof seen) { if (seen[op]) return; seen[op] = 1; }
  gdbg("unhandled rendering opcode %u (%zu payload bytes)", op, n);
}

static void do_rop(uint32_t op, const uint8_t* a, size_t n) {
  float f[16];
  g_ncmds++;

  switch (op) {

  // ---- immediate mode ------------------------------------------------------
  // Only the "v" forms ever appear on the wire: glVertex3f(x,y,z) is encoded as Vertex3fv.
  case ROP_Begin:  if (n >= 4) { glb_begin(r32(a)); g_content = 1; }  return;
  case ROP_End:    glb_end();                                          return;

  case ROP_Vertex2fv: if (n >= 8)  { glb_vertex2f(rf32(a), rf32(a + 4)); }  return;
  case ROP_Vertex3fv: if (n >= 12) { glb_vertex3f(rf32(a), rf32(a + 4), rf32(a + 8)); } return;
  case ROP_Vertex4fv: if (n >= 16) {                                     // project through w
                        float w = rf32(a + 12); if (w == 0.0f) w = 1.0f;
                        glb_vertex3f(rf32(a) / w, rf32(a + 4) / w, rf32(a + 8) / w);
                      } return;
  case ROP_Vertex2dv: if (n >= 16) { glb_vertex2f((float)rf64(a), (float)rf64(a + 8)); } return;
  case ROP_Vertex3dv: if (n >= 24) { glb_vertex3f((float)rf64(a), (float)rf64(a + 8),
                                                  (float)rf64(a + 16)); } return;
  case ROP_Vertex2iv: if (n >= 8)  { glb_vertex2f((float)ri32(a), (float)ri32(a + 4)); } return;
  case ROP_Vertex3iv: if (n >= 12) { glb_vertex3f((float)ri32(a), (float)ri32(a + 4),
                                                  (float)ri32(a + 8)); } return;
  case ROP_Vertex2sv: if (n >= 4)  { glb_vertex2f((float)(int16_t)r16(a),
                                                  (float)(int16_t)r16(a + 2)); } return;
  case ROP_Vertex3sv: if (n >= 6)  { glb_vertex3f((float)(int16_t)r16(a),
                                                  (float)(int16_t)r16(a + 2),
                                                  (float)(int16_t)r16(a + 4)); } return;

  case ROP_Normal3fv: if (n >= 12) { glb_normal3f(rf32(a), rf32(a + 4), rf32(a + 8)); } return;
  case ROP_Normal3dv: if (n >= 24) { glb_normal3f((float)rf64(a), (float)rf64(a + 8),
                                                  (float)rf64(a + 16)); } return;
  case ROP_Normal3bv: if (n >= 3)  { glb_normal3f((int8_t)a[0] / 127.0f, (int8_t)a[1] / 127.0f,
                                                  (int8_t)a[2] / 127.0f); } return;
  case ROP_Normal3sv: if (n >= 6)  { glb_normal3f((int16_t)r16(a) / 32767.0f,
                                                  (int16_t)r16(a + 2) / 32767.0f,
                                                  (int16_t)r16(a + 4) / 32767.0f); } return;
  case ROP_Normal3iv: if (n >= 12) { glb_normal3f((float)ri32(a) / 2147483647.0f,
                                                  (float)ri32(a + 4) / 2147483647.0f,
                                                  (float)ri32(a + 8) / 2147483647.0f); } return;

  case ROP_Color3fv:  if (n >= 12) { glb_color3f(rf32(a), rf32(a + 4), rf32(a + 8)); } return;
  case ROP_Color4fv:  if (n >= 16) { glb_color4f(rf32(a), rf32(a + 4),
                                                 rf32(a + 8), rf32(a + 12)); } return;
  case ROP_Color3dv:  if (n >= 24) { glb_color3f((float)rf64(a), (float)rf64(a + 8),
                                                 (float)rf64(a + 16)); } return;
  case ROP_Color4dv:  if (n >= 32) { glb_color4f((float)rf64(a), (float)rf64(a + 8),
                                                 (float)rf64(a + 16), (float)rf64(a + 24)); } return;
  case ROP_Color3ubv: if (n >= 3)  { glb_color3f(a[0] / 255.0f, a[1] / 255.0f,
                                                 a[2] / 255.0f); } return;
  case ROP_Color4ubv: if (n >= 4)  { glb_color4f(a[0] / 255.0f, a[1] / 255.0f,
                                                 a[2] / 255.0f, a[3] / 255.0f); } return;
  case ROP_Color3bv:  if (n >= 3)  { glb_color3f((int8_t)a[0] / 127.0f, (int8_t)a[1] / 127.0f,
                                                 (int8_t)a[2] / 127.0f); } return;
  case ROP_Color4bv:  if (n >= 4)  { glb_color4f((int8_t)a[0] / 127.0f, (int8_t)a[1] / 127.0f,
                                                 (int8_t)a[2] / 127.0f, (int8_t)a[3] / 127.0f); }
                      return;
  case ROP_Color3sv:  if (n >= 6)  { glb_color3f(r16(a) / 65535.0f, r16(a + 2) / 65535.0f,
                                                 r16(a + 4) / 65535.0f); } return;
  case ROP_Color4sv:  if (n >= 8)  { glb_color4f(r16(a) / 65535.0f, r16(a + 2) / 65535.0f,
                                                 r16(a + 4) / 65535.0f, r16(a + 6) / 65535.0f); }
                      return;
  case ROP_Color3iv:  if (n >= 12) { glb_color3f((float)r32(a) / 4294967295.0f,
                                                 (float)r32(a + 4) / 4294967295.0f,
                                                 (float)r32(a + 8) / 4294967295.0f); } return;
  case ROP_Color4iv:  if (n >= 16) { glb_color4f((float)r32(a) / 4294967295.0f,
                                                 (float)r32(a + 4) / 4294967295.0f,
                                                 (float)r32(a + 8) / 4294967295.0f,
                                                 (float)r32(a + 12) / 4294967295.0f); } return;

  case ROP_TexCoord1fv: if (n >= 4)  { glb_texcoord2f(rf32(a), 0.0f); } return;
  case ROP_TexCoord2fv:
  case ROP_TexCoord3fv:
  case ROP_TexCoord4fv: if (n >= 8)  { glb_texcoord2f(rf32(a), rf32(a + 4)); } return;
  case ROP_TexCoord2dv: if (n >= 16) { glb_texcoord2f((float)rf64(a), (float)rf64(a + 8)); } return;

  case ROP_EdgeFlagv:  return;                                            // no effect here

  // ---- display lists -------------------------------------------------------
  case ROP_CallList:   if (n >= 4) { glb_call_list(r32(a)); g_content = 1; } return;
  case ROP_CallLists: {
    if (n < 8) return;
    int32_t  cnt  = ri32(a);
    uint32_t type = r32(a + 4);
    const uint8_t* q = a + 8; size_t left = n - 8;
    size_t sz;
    switch (type) {
      case 0x1400: case 0x1401: sz = 1; break;                            // BYTE / UNSIGNED_BYTE
      case 0x1402: case 0x1403: sz = 2; break;                            // SHORT / UNSIGNED_SHORT
      case 0x1404: case 0x1405: case 0x1406: sz = 4; break;               // INT / UINT / FLOAT
      default: return;
    }
    for (int32_t i = 0; i < cnt && left >= sz; i++) {
      uint32_t id;
      if      (sz == 1)        id = *q;
      else if (sz == 2)        id = r16(q);
      else if (type == 0x1406) id = (uint32_t)rf32(q);
      else                     id = r32(q);
      q += sz; left -= sz;
      glb_call_list(id);
    }
    g_content = 1;
    return;
  }
  case ROP_ListBase:   return;                                            // glListBase: unused

  // ---- matrix stack --------------------------------------------------------
  case ROP_MatrixMode:   if (n >= 4)  glb_matrix_mode(r32(a));            return;
  case ROP_LoadIdentity: glb_load_identity();                             return;
  case ROP_PushMatrix:   glb_push_matrix();                               return;
  case ROP_PopMatrix:    glb_pop_matrix();                                return;
  case ROP_LoadMatrixf:  if (n >= 64) { read_floats(a, n, 16, f); glb_load_matrix(f); } return;
  case ROP_MultMatrixf:  if (n >= 64) { read_floats(a, n, 16, f); glb_mult_matrix(f); } return;
  case ROP_LoadMatrixd:  if (n >= 128) { for (int i = 0; i < 16; i++) f[i] = (float)rf64(a + i * 8);
                                         glb_load_matrix(f); }            return;
  case ROP_MultMatrixd:  if (n >= 128) { for (int i = 0; i < 16; i++) f[i] = (float)rf64(a + i * 8);
                                         glb_mult_matrix(f); }            return;
  // glTranslatef/glRotatef/glScalef keep FLOAT32 on the wire; the "d" entry points get their
  // own opcodes and carry FLOAT64.  glFrustum/glOrtho only exist in the double form.
  case ROP_Translatef: if (n >= 12) { glb_translate(rf32(a), rf32(a + 4), rf32(a + 8)); } return;
  case ROP_Translated: if (n >= 24) { glb_translate((float)rf64(a), (float)rf64(a + 8),
                                                    (float)rf64(a + 16)); } return;
  case ROP_Scalef:     if (n >= 12) { glb_scale(rf32(a), rf32(a + 4), rf32(a + 8)); } return;
  case ROP_Scaled:     if (n >= 24) { glb_scale((float)rf64(a), (float)rf64(a + 8),
                                                (float)rf64(a + 16)); } return;
  case ROP_Rotatef:    if (n >= 16 && angle_probe_on()) {
                         // Every frame issues the same sequence of glRotatef calls, so pinning
                         // the probe to a fixed position in that sequence follows ONE gear.
                         // (Sampling every Nth call across frames instead aliases between gears
                         // whose angles differ by a factor of two, which reads as violent
                         // jitter even when the animation is perfectly even — that is what the
                         // raw [angle] trace below shows, and why [swangle] exists.)
                         if (g_frame_rots == ANGLE_PROBE_SLOT) g_frame_angle = rf32(a);
                         g_frame_rots++;

                         static double t0; static int nrot;
                         struct timespec ts_; clock_gettime(CLOCK_MONOTONIC, &ts_);
                         double now_ = ts_.tv_sec + ts_.tv_nsec/1e9; if (t0==0) t0=now_;
                         if (nrot++ < 24 || (nrot % 60) == 0)
                           fprintf(stderr, "[angle] #%d t=%.3f deg=%.2f axis=(%.0f,%.0f,%.0f)\n", nrot, now_-t0, (double)rf32(a), (double)rf32(a+4), (double)rf32(a+8), (double)rf32(a+12));
                       }
                       if (n >= 16) { glb_rotate(rf32(a), rf32(a + 4),
                                                 rf32(a + 8), rf32(a + 12)); } return;
  case ROP_Rotated:    if (n >= 32) { glb_rotate((float)rf64(a), (float)rf64(a + 8),
                                                 (float)rf64(a + 16), (float)rf64(a + 24)); }
                       return;
  case ROP_Frustum:    if (n >= 48) { glb_frustum((float)rf64(a),      (float)rf64(a + 8),
                                                  (float)rf64(a + 16), (float)rf64(a + 24),
                                                  (float)rf64(a + 32), (float)rf64(a + 40)); }
                       return;
  case ROP_Ortho:      if (n >= 48) { glb_ortho((float)rf64(a),      (float)rf64(a + 8),
                                                (float)rf64(a + 16), (float)rf64(a + 24),
                                                (float)rf64(a + 32), (float)rf64(a + 40)); }
                       return;

  // ---- framebuffer ---------------------------------------------------------
  case ROP_Viewport:   if (n >= 16) { glb_viewport(ri32(a), ri32(a + 4),
                                                   ri32(a + 8), ri32(a + 12)); } return;
  case ROP_Clear:      if (n >= 4)  { glb_clear_current(r32(a)); g_content = 1; } return;
  case ROP_ClearColor: if (n >= 16) { glb_clear_color(rf32(a), rf32(a + 4),
                                                      rf32(a + 8), rf32(a + 12)); } return;
  case ROP_ClearDepth: if (n >= 8)  { glb_clear_depth_value((float)rf64(a)); } return;
  case ROP_ClearIndex: case ROP_ClearAccum: case ROP_ClearStencil:
  case ROP_StencilMask: case ROP_ColorMask:
  case ROP_StencilFunc: case ROP_StencilOp:
  case ROP_DrawBuffer:  case ROP_ReadBuffer:
  case ROP_AlphaFunc:   case ROP_LogicOp:
  case ROP_Scissor:     case ROP_PolygonMode: case ROP_PolygonOffset:
  case ROP_Hint:        case ROP_DepthRange:
  case ROP_PushAttrib:  case ROP_PopAttrib:
    return;                                                               // accepted, no-op

  case ROP_DepthMask:  if (n >= 1) { glb_depth_mask(a[0] != 0); }         return;  // BOOL + 3 pad
  case ROP_DepthFunc:  if (n >= 4) { glb_depth_func(r32(a)); }            return;

  // ---- state ---------------------------------------------------------------
  case ROP_Enable:     if (n >= 4) { glb_enable(r32(a)); }                return;
  case ROP_Disable:    if (n >= 4) { glb_disable(r32(a)); }               return;
  case ROP_ShadeModel: if (n >= 4) { glb_shade_model(r32(a)); }           return;
  case ROP_CullFace:   if (n >= 4) { glb_cull_face(r32(a)); }             return;
  case ROP_FrontFace:  if (n >= 4) { glb_front_face(r32(a)); }            return;
  case ROP_BlendFunc:  if (n >= 8) { glb_blend_func(r32(a), r32(a + 4)); } return;
  case ROP_LineWidth:  return;
  case ROP_PointSize:  if (n >= 4) { glb_point_size(rf32(a)); }           return;
  case ROP_ColorMaterial: if (n >= 8) { glb_color_material(r32(a), r32(a + 4)); } return;

  // ---- lighting ------------------------------------------------------------
  // Lightfv / Materialfv: { ENUM light|face; ENUM pname; FLOAT32 params[] }.  The value count
  // is implied by pname, but deriving it from the command length is equivalent and safer.
  case ROP_Lightfv:
    if (n >= 8) { read_floats(a + 8, n - 8, 4, f); glb_light(r32(a), r32(a + 4), f); }
    return;
  case ROP_Lightf:
    if (n >= 12) { f[0] = rf32(a + 8); f[1] = f[2] = f[3] = 0.0f;
                   glb_light(r32(a), r32(a + 4), f); }
    return;
  case ROP_Lighti:
    if (n >= 12) { f[0] = (float)ri32(a + 8); f[1] = f[2] = f[3] = 0.0f;
                   glb_light(r32(a), r32(a + 4), f); }
    return;
  case ROP_Lightiv:
    if (n >= 8) { int cnt = (int)((n - 8) / 4); if (cnt > 4) cnt = 4;
                  for (int i = 0; i < 4; i++) f[i] = (i < cnt) ? (float)ri32(a + 8 + i * 4) : 0.0f;
                  glb_light(r32(a), r32(a + 4), f); }
    return;
  case ROP_Materialfv:
    if (n >= 8) { read_floats(a + 8, n - 8, 4, f); glb_material(r32(a), r32(a + 4), f); }
    return;
  case ROP_Materialf:
    if (n >= 12) { f[0] = rf32(a + 8); f[1] = f[2] = f[3] = 0.0f;
                   glb_material(r32(a), r32(a + 4), f); }
    return;
  case ROP_Materiali:
    if (n >= 12) { f[0] = (float)ri32(a + 8); f[1] = f[2] = f[3] = 0.0f;
                   glb_material(r32(a), r32(a + 4), f); }
    return;
  case ROP_Materialiv:
    if (n >= 8) { int cnt = (int)((n - 8) / 4); if (cnt > 4) cnt = 4;
                  for (int i = 0; i < 4; i++) f[i] = (i < cnt) ? (float)ri32(a + 8 + i * 4) : 0.0f;
                  glb_material(r32(a), r32(a + 4), f); }
    return;
  // LightModelfv: { ENUM pname; FLOAT32 params[] }  (one arg fewer than Lightfv).
  case ROP_LightModelfv:
    if (n >= 4) { read_floats(a + 4, n - 4, 4, f); glb_light_model(r32(a), f); }
    return;
  case ROP_LightModelf:
    if (n >= 8) { f[0] = rf32(a + 4); f[1] = f[2] = f[3] = 0.0f; glb_light_model(r32(a), f); }
    return;
  case ROP_LightModeli:
    if (n >= 8) { f[0] = (float)ri32(a + 4); f[1] = f[2] = f[3] = 0.0f;
                  glb_light_model(r32(a), f); }
    return;
  case ROP_LightModeliv:
    if (n >= 4) { int cnt = (int)((n - 4) / 4); if (cnt > 4) cnt = 4;
                  for (int i = 0; i < 4; i++) f[i] = (i < cnt) ? (float)ri32(a + 4 + i * 4) : 0.0f;
                  glb_light_model(r32(a), f); }
    return;

  // ---- textures ------------------------------------------------------------
  case ROP_BindTexture:   if (n >= 8)  { glb_bind_texture(r32(a), r32(a + 4)); } return;
  case ROP_TexParameteri: if (n >= 12) { glb_tex_parameter_i(r32(a), r32(a + 4),
                                                             ri32(a + 8)); } return;
  case ROP_TexParameterf: if (n >= 12) { glb_tex_parameter_i(r32(a), r32(a + 4),
                                                             (int)rf32(a + 8)); } return;

  default:
    unknown_rop(op, n);
    return;
  }
}

// ---------------------------------------------------------------------------
// GLXRender: walk the batch
// ---------------------------------------------------------------------------
void glxd_render(const uint8_t* p, size_t n) {
  if (!g_ready || !p) return;
  while (n >= 4) {
    uint32_t len = r16(p);
    uint32_t op  = r16(p + 2);
    // `len` counts the 4-byte header and is padded to a multiple of 4.
    if (len < 4 || (len & 3) || len > n) {
      gdbg("render batch desync: len=%u op=%u remaining=%zu", len, op, n);
      return;
    }
    do_rop(op, p + 4, len - 4);
    p += len; n -= len;
  }
  if (n) gdbg("render batch: %zu trailing bytes ignored", n);
}

// ---------------------------------------------------------------------------
// GLXRenderLarge: reassemble, then dispatch the single oversized command.
// The reassembled stream starts with { CARD32 length; CARD32 opcode } — an 8-byte header,
// unlike the 4-byte header used inside GLXRender.
// ---------------------------------------------------------------------------
static void large_reset(void) { g_large_len = 0; g_large_next = 0; g_large_total = 0; }

void glxd_render_large(const uint8_t* d, size_t dlen) {
  if (!g_ready || dlen < 12) return;
  uint16_t num   = r16(d + 4);
  uint16_t total = r16(d + 6);
  uint32_t bytes = r32(d + 8);
  const uint8_t* data = d + 12;
  size_t avail = dlen - 12;
  if (bytes > avail) bytes = (uint32_t)avail;

  if (num == 1) { large_reset(); g_large_total = total; g_large_next = 1; }
  if (num != g_large_next || total != g_large_total) {
    gdbg("RenderLarge out of order: got %u/%u, expected %u/%u",
         num, total, g_large_next, g_large_total);
    large_reset();
    return;
  }

  if (g_large_len + bytes > g_large_cap) {
    size_t cap = g_large_cap ? g_large_cap : 65536;
    while (cap < g_large_len + bytes) cap *= 2;
    uint8_t* nb = (uint8_t*)realloc(g_large, cap);
    if (!nb) { large_reset(); return; }
    g_large = nb; g_large_cap = cap;
  }
  memcpy(g_large + g_large_len, data, bytes);
  g_large_len += bytes;
  g_large_next++;

  if (num < total) return;                            // more chunks to come

  if (g_large_len >= 8) {
    uint32_t len = r32(g_large);
    uint32_t op  = r32(g_large + 4);
    if (len > g_large_len) len = (uint32_t)g_large_len;
    gdbg("RenderLarge complete: op=%u len=%u (%zu bytes reassembled)", op, len, g_large_len);
    do_rop(op, g_large + 8, len - 8);
  }
  large_reset();
}

// ---------------------------------------------------------------------------
// GL single commands routed here
// ---------------------------------------------------------------------------
uint32_t glxd_gen_lists(uint32_t range) {
  if (!g_ready) return 0;
  return glb_gen_lists((int)range);
}
void glxd_new_list(uint32_t list, uint32_t mode) {
  if (!g_ready) return;
  glb_new_list(list, mode);
}
void glxd_end_list(void) {
  if (!g_ready) return;
  glb_end_list();
}
void glxd_delete_lists(uint32_t list, uint32_t range) {
  if (!g_ready) return;
  glb_delete_lists(list, (int)range);
}
int glxd_is_list(uint32_t list)  { return g_ready ? glb_is_list(list)  : 0; }
int glxd_is_enabled(uint32_t cap){ return g_ready ? glb_is_enabled(cap) : 0; }

void glxd_swap(uint8_t* rgba, int w, int h) {
  if (!g_ready) return;
  if (angle_probe_on()) {
    // `shown` distinguishes a swap that reaches the display from one the presenter throttled
    // away: on the readback path only the frames that get read back are ever seen, and the
    // ratio between the two is the whole story of the burst-then-lurch stutter.
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[swangle] t=%.6f deg=%.3f shown=%d\n",
            (double)ts.tv_sec + (double)ts.tv_nsec / 1e9, (double)g_frame_angle, rgba ? 1 : 0);
    g_frame_rots = 0;
  }
  glb_swap();
  if (!rgba || w <= 0 || h <= 0) return;
  if (glb_readback(rgba, w, h) != 0) return;
  // The GL clear colour is (0,0,0,0) by default; the X framebuffer is composited as
  // straight RGBA, so force the frame opaque.
  size_t px = (size_t)w * (size_t)h;
  for (size_t i = 0; i < px; i++) rgba[i * 4 + 3] = 0xFF;
}
