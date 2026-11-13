// capresent.h — the CALayer/IOSurface presentation path, as an alternative to the Metal one.
//
// THE QUESTION THIS EXISTS TO ANSWER, and nothing else: is it cheaper to hand WindowServer an
// IOSurface and let it composite, than to upload the compositor's framebuffer into a Metal
// staging texture and blit it onto a CAMetalLayer drawable ourselves?
//
// Today's path, per presented frame, counts THREE full-frame CPU passes over the pixels:
//     1. convert_buffer()          shm pool          -> surface->rgba     (vImageCopyBuffer)
//     2. recomposite()             surface->rgba     -> g_rgba            (memcpy)
//     3. gpuwin_present_fmt()      g_rgba            -> MTLTexture stage  (replaceRegion)
//   + a GPU render pass that samples the stage onto the drawable, and a blocking
//     -[CAMetalLayer nextDrawable] on the compositor thread.
//
// This path replaces 2 and 3 with ONE pass into an IOSurface that WindowServer reads directly,
// and deletes the render pass and the drawable wait outright. Under WLSRV_CALAYER=2 it also
// folds pass 1 into it for the single-surface case, leaving ONE pass in total.
//
// EVERYTHING HERE IS OFF UNLESS WLSRV_CALAYER IS SET. The Metal path is not modified, the
// headless builds link the stubs in wlhead_stub.c, and the A/B is one environment variable on
// one binary — the project's own standard for a speedup you can show going away.
#ifndef CAPRESENT_H
#define CAPRESENT_H

#include <stdint.h>
#include <stddef.h>

// 0 = off (Metal path), 1 = composite into an IOSurface, 2 = 1 plus convert straight into it.
// Read once from WLSRV_CALAYER.
int capres_mode(void);

// Is the CA path both requested AND usable (window server reachable, layer installed)?
// Returns 0 in the headless build no matter what WLSRV_CALAYER says.
int capres_enabled(void);

// Take a free IOSurface of w x h BGRA and lock it for CPU writing. Returns its base address and
// writes the ROW STRIDE — which is NOT w*4: IOSurface aligns rows, and a caller that assumes a
// packed framebuffer will produce a sheared image. NULL if unavailable (caller falls back).
uint8_t* capres_acquire(uint32_t w, uint32_t h, size_t* stride);

// Unlock the surface taken by the last capres_acquire() and hand it to the layer. 0 on success.
int capres_publish(void);

// The most recently PUBLISHED surface's pixels, read-only, or NULL if nothing has been published
// or the size no longer matches. Under WLSRV_CALAYER=2 the toplevel's own retained copy is not
// maintained — the shm buffer goes straight to the screen — so this is how the compositor
// recovers that copy the first time a popup or dialog appears and the frame has to be assembled
// from more than one surface again.
const uint8_t* capres_last_published(uint32_t w, uint32_t h, size_t* stride);

// (There was a capres_wait_vsync() here, to gate wl_surface.frame on a CADisplayLink. It is
// gone: S2 measured on screen that -[CAMetalLayer nextDrawable] ALREADY applies vsync-paced
// back-pressure — 99.5-99.8% of frames land on the very next refresh and frame-period p95 is
// 1.05-1.1x p50. The bimodal tail that motivated it is a headless artefact. See capresent.m.)

// One-line summary of what this path actually did — surfaces allocated, frames published,
// frames that found no free surface, and the copies-per-frame the mode implies. Printed at exit
// under WLSRV_PROF=1.
void capres_report(void);

// Called by window.m on the main thread while building the window: returns the CALayer to
// install as the content view's layer, or NULL when the CA path is off (use CAMetalLayer).
void* capres_make_layer(void);

#endif
