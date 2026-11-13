// test_capresent.m — does the IOSurface path put the RIGHT bytes where WindowServer reads them?
//
// This is deliberately NOT an on-screen test. S2 holds the GUI lock and owns on-screen proof; a
// second shim_gui would silently steal their Wayland client through the single global socket
// path. What this can prove without a window is the half that is mine: the bytes handed to the
// compositor are the client's pixels, at the surface's own stride, in a surface that actually
// rotates.
//
// EVERY RUNG HAS A NEGATIVE CONTROL, because the two ways this path fails silently both look
// like working software:
//
//   A  A PACKED write into an aligned surface. IOSurface rows are 64-byte aligned; writing at
//      width*4 shears the image progressively down the frame. On a 1024-wide surface the stride
//      happens to already be 4096 and NOTHING GOES WRONG — so the control uses a width whose
//      packed and aligned strides differ, or it proves nothing at all. The test asserts the two
//      strides really do differ before trusting the control.
//   B  Red and blue swapped. This is the exact mutation "keep BGRA end to end" invites, and a
//      blue-grey window with R and B exchanged does not look broken.
//   C  A surface that was never written. If the checker passes a black frame, it passes anything.
//
// If any control comes out the "good" way the whole test FAILS: a control that cannot fail means
// the instrument is blind, not that the code is extra correct.
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "capresent.h"

static int g_fail;
static void ok(int cond, const char* what) {
  printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) g_fail = 1;
}

// A pattern with no symmetry a swap or a shear could survive: every channel is a different
// function of (x,y), and the row term is prime-strided so a one-row shear changes every pixel.
static void pattern_px(int x, int y, uint8_t out[4]) {
  out[0] = (uint8_t)(x * 7 + y * 3);           // B
  out[1] = (uint8_t)(x * 13 + y * 29);         // G
  out[2] = (uint8_t)(x * 31 + y * 17);         // R
  out[3] = 0xFF;                               // A
}

static void fill(uint8_t* base, size_t stride, int w, int h, int swap_rb) {
  for (int y = 0; y < h; y++) {
    uint8_t* row = base + (size_t)y * stride;
    for (int x = 0; x < w; x++) {
      uint8_t p[4]; pattern_px(x, y, p);
      if (swap_rb) { uint8_t t = p[0]; p[0] = p[2]; p[2] = t; }
      memcpy(row + x * 4, p, 4);
    }
  }
}

// Read the surface back at ITS stride and compare to the pattern. Returns the number of wrong
// pixels, so a control can assert "many", not merely "nonzero".
static long check(const uint8_t* base, size_t stride, int w, int h) {
  long bad = 0;
  for (int y = 0; y < h; y++) {
    const uint8_t* row = base + (size_t)y * stride;
    for (int x = 0; x < w; x++) {
      uint8_t p[4]; pattern_px(x, y, p);
      if (memcmp(row + x * 4, p, 4) != 0) bad++;
    }
  }
  return bad;
}

int main(void) { @autoreleasepool {
  // The mode has to be on before anything else: capres_mode() latches its value on first read.
  if (!getenv("RDCA") && !getenv("WLSRV_CALAYER")) setenv("RDCA", "2", 1);
  printf("test_capresent: mode %d\n", capres_mode());

  void* layer = capres_make_layer();
  ok(layer != NULL, "capres_make_layer() returns a layer with no window");
  ok(capres_enabled(), "capres_enabled() is true once the layer exists");
  if (!capres_enabled()) { printf("cannot continue\n"); return 1; }

  // 1000, not 1024: a power-of-two width's packed stride is ALREADY 64-byte aligned, so control A
  // would silently test nothing. This is the whole reason the width is odd-looking.
  const int W = 1000, H = 600;
  size_t stride = 0;
  uint8_t* base = capres_acquire((uint32_t)W, (uint32_t)H, &stride);
  ok(base != NULL, "capres_acquire() returns a base address");
  if (!base) return 1;
  printf("  surface %dx%d stride %zu (packed would be %d)\n", W, H, stride, W * 4);
  ok(stride >= (size_t)W * 4, "stride is at least the packed row");
  ok(stride % 64 == 0, "stride is 64-byte aligned");
  ok(stride != (size_t)W * 4,
     "PRECONDITION for control A: aligned and packed strides actually differ");

  fill(base, stride, W, H, 0);
  ok(capres_publish() == 0, "capres_publish() succeeds");

  size_t rstride = 0;
  const uint8_t* rd = capres_last_published((uint32_t)W, (uint32_t)H, &rstride);
  ok(rd != NULL, "capres_last_published() returns the frame just published");
  ok(rd == base, "it is the same surface that was written (not a copy)");
  ok(rstride == stride, "and it reports the same stride");
  long bad = rd ? check(rd, rstride, W, H) : -1;
  ok(bad == 0, "every pixel of the published frame matches the source pattern");
  if (bad > 0) printf("       %ld wrong pixels\n", bad);

  // ---- A: a PACKED write into an aligned surface must be caught -----------------------------
  base = capres_acquire((uint32_t)W, (uint32_t)H, &stride);
  if (base) {
    memset(base, 0, stride * (size_t)H);
    fill(base, (size_t)W * 4, W, H, 0);            // deliberately the WRONG stride
    capres_publish();
    rd = capres_last_published((uint32_t)W, (uint32_t)H, &rstride);
    long b = rd ? check(rd, rstride, W, H) : -1;
    ok(b > (long)W * H / 100,
       "control A: a packed-stride write is REJECTED (the shear is detected)");
    printf("       %ld wrong pixels of %d\n", b, W * H);
  }

  // ---- B: red and blue swapped must be caught -----------------------------------------------
  base = capres_acquire((uint32_t)W, (uint32_t)H, &stride);
  if (base) {
    fill(base, stride, W, H, 1);
    capres_publish();
    rd = capres_last_published((uint32_t)W, (uint32_t)H, &rstride);
    long b = rd ? check(rd, rstride, W, H) : -1;
    ok(b > (long)W * H / 100, "control B: an R/B swap is REJECTED");
    printf("       %ld wrong pixels of %d\n", b, W * H);
  }

  // ---- C: an unwritten surface must be caught -----------------------------------------------
  base = capres_acquire((uint32_t)W, (uint32_t)H, &stride);
  if (base) {
    memset(base, 0, stride * (size_t)H);
    capres_publish();
    rd = capres_last_published((uint32_t)W, (uint32_t)H, &rstride);
    long b = rd ? check(rd, rstride, W, H) : -1;
    ok(b > (long)W * H / 100, "control C: a never-written (black) frame is REJECTED");
  }

  // ---- the ring really rotates ---------------------------------------------------------------
  // -[CALayer setContents:] with the pointer already there is a NO-OP. A path that reuses one
  // surface therefore displays frame 1 forever while reporting hundreds of publishes — the
  // single most plausible silent failure of this design, and invisible on a static test image.
  const uint8_t* seen[4] = {0};
  int distinct = 0;
  for (int i = 0; i < 3; i++) {
    uint8_t* b2 = capres_acquire((uint32_t)W, (uint32_t)H, &stride);
    if (!b2) break;
    fill(b2, stride, W, H, 0);
    capres_publish();
    int isnew = 1;
    for (int k = 0; k < distinct; k++) if (seen[k] == b2) isnew = 0;
    if (isnew && distinct < 4) seen[distinct++] = b2;
  }
  ok(distinct == 3, "three consecutive publishes use three DISTINCT surfaces "
                    "(so setContents: is never a no-op)");
  printf("       %d distinct surfaces across 3 publishes\n", distinct);

  // ---- a resize must reallocate rather than hand back the old geometry ------------------------
  base = capres_acquire(640, 480, &stride);
  ok(base != NULL, "acquire at a new size succeeds");
  ok(capres_last_published(1000, 600, &rstride) == NULL,
     "after a resize the old-geometry readback is refused rather than returning wrong pixels");
  if (base) capres_publish();

  capres_report();
  printf("test_capresent: %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail;
}}
