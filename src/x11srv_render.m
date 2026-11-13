// x11srv_render.m — render bridge for the X11 server.
//
// The X11 server core (x11srv.c) is plain C and rasterizes every drawing request
// into a CPU-side RGBA8 framebuffer. To actually appear on screen that buffer
// must reach a Metal-backed NSWindow, which lives in window.m behind the C-ABI
// gpuwin_* entry points (Cocoa is main-thread-only; those calls marshal to main).
//
// This file is the only Objective-C translation unit the server needs: it simply
// forwards x11rnd_* -> gpuwin_*. Keeping it separate means the server core has no
// dependency on gpu.h / Metal, and the presentation backend can be replaced (e.g.
// a headless PNG dumper) without touching the protocol code.
#import <Foundation/Foundation.h>
#include "gpu.h"
#include "glbridge.h"
#include "x11srv.h"

void x11rnd_open(uint32_t w, uint32_t h) {
  (void)gpuwin_open((uint64_t)w, (uint64_t)h);
}

void x11rnd_present(const uint8_t* rgba, uint32_t w, uint32_t h) {
  if (!rgba || !w || !h) return;
  (void)gpuwin_present_fmt(rgba, (uint64_t)w, (uint64_t)h, /*bgra=*/0);
}

void x11rnd_present_bgra(const uint8_t* bgra, uint32_t w, uint32_t h) {
  if (!bgra || !w || !h) return;
  (void)gpuwin_present_fmt(bgra, (uint64_t)w, (uint64_t)h, /*bgra=*/1);
}

// Zero-copy present of the GL bridge's finished frame (window.m). Used instead of
// x11rnd_present() when the GL drawable IS the window being presented: the frame is already a
// Metal texture, so nothing goes through a CPU framebuffer. 0 on success, negative errno
// otherwise (-11 = no drawable free, frame skipped; anything else = path unavailable).
extern long gpuwin_present_texture(void* mtl_texture, void* mtl_queue, uint64_t w, uint64_t h);

int x11rnd_present_gl(uint32_t w, uint32_t h) {
  void* tex = glb_frame_texture();
  if (!tex || !w || !h) return -22;
  return (int)gpuwin_present_texture(tex, glb_metal_queue(), (uint64_t)w, (uint64_t)h);
}

int x11rnd_poll(void) {
  return gpuwin_poll();
}

// Live cursor for the guest's QueryPointer (window-local, top-left origin).
extern void gpuwin_cursor(int* x, int* y);
void x11srv_pointer(int* x, int* y) { gpuwin_cursor(x, y); }
