// glbridge.h — fixed-function OpenGL (GL 1.x) semantics implemented on Metal.
//
// WHAT THIS IS
//   The indirect-GLX wire decoder turns a Linux client's batched GL request stream into calls on
//   this C API; glbridge renders it with Metal into an offscreen MTLTexture. The texture (or a
//   readback of it) is handed to the window layer (`gpuwin_present`, see gpu.h).
//
// ENUMS
//   Every `uint32_t` enum parameter takes the *raw OpenGL enum value*, so the wire decoder can
//   forward what it read off the socket untouched. The GLB_* constants below are those values.
//
// THREADING
//   A GL context is single-threaded; so is this module. Call every glb_* entry point from one
//   thread (the X-server / guest thread). Nothing here touches Cocoa, so it does not need main.
//
// FRAME MODEL (performance-critical, see glbridge.m)
//   One MTLRenderCommandEncoder per FRAME. glb_clear ends the pass and reopens a cleared one;
//   glb_swap ends + commits without blocking. Vertex/uniform data is bump-allocated out of a
//   triple-buffered shared-memory ring bounded by a semaphore, so nothing is allocated per draw.
#ifndef LINUX_SHIM_GLBRIDGE_H
#define LINUX_SHIM_GLBRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ GL enum values ---------- */
/* clear mask bits */
#define GLB_DEPTH_BUFFER_BIT              0x00000100u
#define GLB_COLOR_BUFFER_BIT              0x00004000u

/* primitives */
#define GLB_POINTS                        0x0000u
#define GLB_LINES                         0x0001u
#define GLB_LINE_LOOP                     0x0002u
#define GLB_LINE_STRIP                    0x0003u
#define GLB_TRIANGLES                     0x0004u
#define GLB_TRIANGLE_STRIP                0x0005u
#define GLB_TRIANGLE_FAN                  0x0006u
#define GLB_QUADS                         0x0007u
#define GLB_QUAD_STRIP                    0x0008u
#define GLB_POLYGON                       0x0009u

/* matrix modes */
#define GLB_MODELVIEW                     0x1700u
#define GLB_PROJECTION                    0x1701u
#define GLB_TEXTURE                       0x1702u

/* capabilities */
#define GLB_CULL_FACE                     0x0B44u
#define GLB_LIGHTING                      0x0B50u
#define GLB_COLOR_MATERIAL                0x0B57u
#define GLB_DEPTH_TEST                    0x0B71u
#define GLB_NORMALIZE                     0x0BA1u
#define GLB_BLEND                         0x0BE2u
#define GLB_TEXTURE_2D                    0x0DE1u
#define GLB_LIGHT0                        0x4000u   /* GLB_LIGHT0 + i, i in [0,7] */

/* comparison / depth funcs */
#define GLB_NEVER                         0x0200u
#define GLB_LESS                          0x0201u
#define GLB_EQUAL                         0x0202u
#define GLB_LEQUAL                        0x0203u
#define GLB_GREATER                       0x0204u
#define GLB_NOTEQUAL                      0x0205u
#define GLB_GEQUAL                        0x0206u
#define GLB_ALWAYS                        0x0207u

/* faces / winding / shading */
#define GLB_FRONT                         0x0404u
#define GLB_BACK                          0x0405u
#define GLB_FRONT_AND_BACK                0x0408u
#define GLB_CW                            0x0900u
#define GLB_CCW                           0x0901u
#define GLB_FLAT                          0x1D00u
#define GLB_SMOOTH                        0x1D01u

/* blend factors */
#define GLB_ZERO                          0x0000u
#define GLB_ONE                           0x0001u
#define GLB_SRC_COLOR                     0x0300u
#define GLB_ONE_MINUS_SRC_COLOR           0x0301u
#define GLB_SRC_ALPHA                     0x0302u
#define GLB_ONE_MINUS_SRC_ALPHA           0x0303u
#define GLB_DST_ALPHA                     0x0304u
#define GLB_ONE_MINUS_DST_ALPHA           0x0305u
#define GLB_DST_COLOR                     0x0306u
#define GLB_ONE_MINUS_DST_COLOR           0x0307u
#define GLB_SRC_ALPHA_SATURATE            0x0308u

/* light / material parameter names */
#define GLB_AMBIENT                       0x1200u
#define GLB_DIFFUSE                       0x1201u
#define GLB_SPECULAR                      0x1202u
#define GLB_POSITION                      0x1203u
#define GLB_EMISSION                      0x1600u
#define GLB_SHININESS                     0x1601u
#define GLB_AMBIENT_AND_DIFFUSE           0x1602u

/* light model parameter names */
#define GLB_LIGHT_MODEL_LOCAL_VIEWER      0x0B51u
#define GLB_LIGHT_MODEL_TWO_SIDE          0x0B52u
#define GLB_LIGHT_MODEL_AMBIENT           0x0B53u

/* display list modes */
#define GLB_COMPILE                       0x1300u
#define GLB_COMPILE_AND_EXECUTE           0x1301u

/* texture pixel formats / types accepted by glb_tex_image_2d */
#define GLB_RGB                           0x1907u
#define GLB_RGBA                          0x1908u
#define GLB_LUMINANCE                     0x1909u
#define GLB_UNSIGNED_BYTE                 0x1401u

/* texture parameters */
#define GLB_NEAREST                       0x2600u
#define GLB_LINEAR                        0x2601u
#define GLB_TEXTURE_MAG_FILTER            0x2800u
#define GLB_TEXTURE_MIN_FILTER            0x2801u
#define GLB_TEXTURE_WRAP_S                0x2802u
#define GLB_TEXTURE_WRAP_T                0x2803u
#define GLB_REPEAT                        0x2901u
#define GLB_CLAMP                         0x2900u
#define GLB_CLAMP_TO_EDGE                 0x812Fu

/* ------------------------------------------------------------------------- lifecycle -------- */

/* Create the renderer. `mtl_device_or_null` is an `id<MTLDevice>` (e.g. the one gpu_bridge.m
 * already made) or NULL to create the system default device. Returns 0, or a negative errno.
 * Safe to call twice; the second call is a no-op returning 0. */
int  glb_init(void* mtl_device_or_null);

/* (Re)allocate the offscreen colour+depth targets. Drains the GPU first. 0 / -errno. */
int  glb_resize(int w, int h);

/* Current framebuffer size (either pointer may be NULL). */
void glb_framebuffer_size(int* w, int* h);

/* Drain, release everything. Safe to call when uninitialised. */
void glb_destroy(void);

/* The `id<MTLDevice>` in use (NULL before glb_init). */
void* glb_metal_device(void);

/* The `id<MTLCommandQueue>` frames are submitted on (NULL before glb_init). A presenter that
 * wants to consume glb_frame_texture() without a readback must encode on THIS queue: command
 * buffers within one queue run in commit order, which is what orders the present's read after
 * the frame's writes and before the next frame overwrites the texture. */
void* glb_metal_queue(void);

/* The finished frame as an `id<MTLTexture>` (RGBA8Unorm, MTLStorageModeShared, top-left origin,
 * usage RenderTarget|ShaderRead). Valid after glb_swap(); this is the zero-copy path for the
 * window layer, which can blit it straight into its CAMetalLayer drawable. */
void* glb_frame_texture(void);

/* Copy the finished frame into `rgba` (w*h*4 bytes, tightly packed, top-left origin, R first).
 * Drains the GPU — this is the *only* blocking entry point. Intended for tests and for the
 * gpuwin_present(const uint8_t*) path. Returns 0, or negative errno. If w/h differ from the
 * framebuffer the overlapping top-left region is copied and the rest is left untouched. */
int  glb_readback(uint8_t* rgba, int w, int h);

/* --------------------------------------------------------------------------- frame ---------- */

/* GL-style viewport, bottom-left origin, in framebuffer pixels. */
void glb_viewport(int x, int y, int w, int h);

/* glClear with the clear values supplied inline (the shape the wire decoder finds convenient). */
void glb_clear(uint32_t mask, float r, float g, float b, float a, float depth);

/* glClearColor / glClearDepth / glClear, for a decoder that tracks them separately. */
void glb_clear_color(float r, float g, float b, float a);
void glb_clear_depth_value(float depth);
void glb_clear_current(uint32_t mask);

/* End + commit the frame. Non-blocking: the CPU runs ahead by up to GLB_FRAMES_IN_FLIGHT frames.
 * Returns the (0-based) index of the frame just submitted. */
uint64_t glb_swap(void);

/* Draw calls and vertices submitted in the frame most recently completed by glb_swap(). */
void glb_frame_stats(uint64_t* draws, uint64_t* vertices);

/* --------------------------------------------------------------------- matrix stack --------- */
void glb_matrix_mode(uint32_t mode);          /* GLB_MODELVIEW | GLB_PROJECTION | GLB_TEXTURE */
void glb_load_identity(void);
void glb_load_matrix(const float m[16]);      /* column-major, GL layout */
void glb_mult_matrix(const float m[16]);      /* current = current * m  (GL post-multiply) */
void glb_push_matrix(void);
void glb_pop_matrix(void);
void glb_translate(float x, float y, float z);
void glb_rotate(float angle_deg, float x, float y, float z);
void glb_scale(float x, float y, float z);
void glb_frustum(float l, float r, float b, float t, float n, float f);
void glb_ortho(float l, float r, float b, float t, float n, float f);
/* Read a matrix back (for tests / glGetFloatv). */
void glb_get_matrix(uint32_t mode, float out[16]);

/* ------------------------------------------------------------------- immediate mode --------- */
/* Vertices accumulate into a reused scratch array; glb_end expands fans/quads/polygons to
 * triangles directly into the vertex ring and emits exactly one draw call. */
void glb_begin(uint32_t prim);
void glb_vertex2f(float x, float y);
void glb_vertex3f(float x, float y, float z);
void glb_normal3f(float x, float y, float z);
void glb_color3f(float r, float g, float b);
void glb_color4f(float r, float g, float b, float a);
void glb_texcoord2f(float s, float t);
void glb_end(void);

/* --------------------------------------------------------------------------- state ---------- */
void glb_enable(uint32_t cap);
void glb_disable(uint32_t cap);
int  glb_is_enabled(uint32_t cap);
void glb_depth_func(uint32_t func);
void glb_depth_mask(int write_enabled);
void glb_cull_face(uint32_t face);
void glb_front_face(uint32_t dir);
void glb_shade_model(uint32_t model);
void glb_blend_func(uint32_t src, uint32_t dst);
void glb_point_size(float size);

/* ------------------------------------------------------------------------ lighting ---------- */
/* `light` is GLB_LIGHT0 + i (raw index 0..7 also accepted).
 * GLB_POSITION is transformed by the *current modelview*, exactly as GL does. */
void glb_light(uint32_t light, uint32_t pname, const float* params);
void glb_material(uint32_t face, uint32_t pname, const float* params);
void glb_light_model(uint32_t pname, const float* params);
void glb_color_material(uint32_t face, uint32_t mode);

/* ------------------------------------------------------------------------ textures ---------- */
void glb_gen_textures(int n, uint32_t* ids);
void glb_delete_textures(int n, const uint32_t* ids);
void glb_bind_texture(uint32_t target, uint32_t id);
void glb_tex_image_2d(uint32_t target, int level, int internalformat,
                      int w, int h, int border,
                      uint32_t format, uint32_t type, const void* pixels);
void glb_tex_parameter_i(uint32_t target, uint32_t pname, int param);

/* -------------------------------------------------------------------- display lists --------- */
/* Required by glxgears: each gear is compiled once and re-issued every frame. Calls made between
 * glb_new_list/glb_end_list are *recorded*, and glb_call_list replays them. */
uint32_t glb_gen_lists(int n);                        /* 0 on failure */
void     glb_new_list(uint32_t id, uint32_t mode);    /* GLB_COMPILE | GLB_COMPILE_AND_EXECUTE */
void     glb_end_list(void);
void     glb_call_list(uint32_t id);
void     glb_delete_lists(uint32_t id, int n);
int      glb_is_list(uint32_t id);

#define GLB_FRAMES_IN_FLIGHT 3

#ifdef __cplusplus
}
#endif
#endif /* LINUX_SHIM_GLBRIDGE_H */
