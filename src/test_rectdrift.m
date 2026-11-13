// test_rectdrift.m — pins the invariant that ACQUITS the real-cursor coordinate transform of
// the "pointer drifts ~1px per event with no input" bug.
//
// Build:
//   clang -arch arm64 -O0 -g -ObjC -o test_rectdrift src/test_rectdrift.m src/gpu_bridge.m \
//         -framework Cocoa -framework Metal -framework QuartzCore -Isrc
//
// It #includes window.m so the file-static transform state (publish_rect, gRect*, gWin) is
// reachable, exactly as src/test_pointer.m does.
//
// WHY THIS TEST EXISTS
//
// The drift was blamed on gpuwin_cursor()'s screen->window transform:
//
//     lx = m.x - rx;
//     ly = rh - (m.y - ry);          // flip to top-left origin
//
// With a stationary physical cursor +[NSEvent mouseLocation] is constant, so lx/ly can only
// walk if gRectX/gRectY/gRectH walk. They cannot, and this file is the executable proof:
// publish_rect() is an ABSOLUTE ASSIGNMENT from [gWin convertRectToScreen:contentView.frame],
// never an accumulation, so re-running it on a stationary window is idempotent no matter how
// many times it fires. (It fires a lot: every 16th present via refresh_rect_occasionally().)
//
// NOTE ON WHAT THIS TEST DELIBERATELY DOES *NOT* DO. Unlike test_pointer.m it never calls
// -[NSWindow makeKeyAndOrderFront:] and never activates the app, so no window is ever shown,
// no CGEvent is posted, and the real cursor is never moved. -[NSWindow convertRectToScreen:]
// is well defined on an unordered window, which is all the transform needs. The test is
// therefore safe to run beside a live GUI session and needs no window-server lock.
//
// PHASE A  (default): idempotence of publish_rect(), and constancy of the transform driven by
//                     a FIXED synthetic mouse point — deterministic, independent of where the
//                     physical mouse actually is. Plus a live gpuwin_cursor() stability check
//                     that SKIPS rather than fails if the physical mouse moved mid-run.
//
// PHASE B  (re-exec with RDINPUT_FAKECURSOR set): pins the HARNESS behaviour that is the far
//                     more likely explanation of the reported drift — RDINPUT_FAKECURSOR makes
//                     gpuwin_cursor() a getter with a side effect, advancing one step per READ
//                     and wrapping, and short-circuiting before the transform ever runs. Two
//                     points 1px apart in that list ARE a 1px-per-read walk, by design.
//                     Note shim_wlh reads a DIFFERENT variable (RDWLH_CURSOR, wlhead_stub.c),
//                     so it stays pinned under the same environment — which is exactly the
//                     gui-drifts/headless-does-not asymmetry, with no bug in either.
#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "window.m"

#define CW 1308     // the real guest content size this bug was reported at
#define CH 829

#define ITERS 20000

static int g_fail;

static void hdr(const char* s) { printf("\n===== %s =====\n", s); }

static void ok(const char* what, int pass) {
  printf("  %-58s %s\n", what, pass ? "PASS" : "FAIL");
  if (!pass) g_fail++;
}

// ---------------------------------------------------------------------- PHASE B
// No window is created in this phase ON PURPOSE: gRectValid stays 0, so if gpuwin_cursor()
// still returns the scripted points it can only be because the FAKECURSOR block ran BEFORE the
// rect was ever consulted. That is the claim being pinned.
static int phase_b(void) {
  hdr("PHASE B  RDINPUT_FAKECURSOR — the harness walks the cursor, by design");
  printf("  RDINPUT_FAKECURSOR = \"%s\"\n", getenv("RDINPUT_FAKECURSOR"));
  printf("  gRectValid = %d  (no window was ever created in this phase)\n", gRectValid);

  // The list is "100,100;100,101" — two points one pixel apart vertically.
  int x[8], y[8];
  for (int i = 0; i < 8; i++) gpuwin_cursor(&x[i], &y[i]);

  printf("  eight consecutive reads, NO input of any kind:\n   ");
  for (int i = 0; i < 8; i++) printf(" (%d,%d)", x[i], y[i]);
  printf("\n");

  ok("rect never consulted (gRectValid == 0)", gRectValid == 0);
  ok("reads return the scripted points, not the physical mouse",
     x[0] == 100 && y[0] == 100 && x[1] == 100 && y[1] == 101);
  ok("the value CHANGES between consecutive reads with no input",
     !(x[0] == x[1] && y[0] == y[1]));
  ok("it walks 1px per READ and wraps (a getter with a side effect)",
     y[0] == 100 && y[1] == 101 && y[2] == 100 && y[3] == 101 &&
     y[4] == 100 && y[5] == 101 && y[6] == 100 && y[7] == 101);

  printf("\n  ^ THIS is a 1px-per-event pointer walk with no input, produced entirely by the\n"
         "    test harness and reaching the guest through the ordinary poll path. Before\n"
         "    blaming window.m's transform, check whether RDINPUT_FAKECURSOR was set.\n");
  return g_fail;
}

// ---------------------------------------------------------------------- PHASE A
static int phase_a(const char* self) {
  @autoreleasepool {
    gpuwin_ensure_app();

    // Same style mask and content size as open_on_main(), but NEVER ordered front. See header.
    NSRect content = NSMakeRect(0, 0, (CGFloat)CW, (CGFloat)CH);
    NSWindowStyleMask mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable;
    gWin = [[NSWindow alloc] initWithContentRect:content
                                       styleMask:mask
                                         backing:NSBackingStoreBuffered
                                           defer:NO];
    gWin.title = @"d2 rect-drift probe (never shown)";
    gWin.releasedWhenClosed = NO;
    gWin.contentView.wantsLayer = YES;

    // Let AppKit settle the frame (constrainFrameRect: can shrink it) before the first publish.
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.20]];
    atomic_store(&gFbW, CW);
    atomic_store(&gFbH, CH);

    hdr("PHASE A  geometry");
    publish_rect();
    const double x0 = gRectX, y0 = gRectY, w0 = gRectW, h0 = gRectH;
    NSRect fr = gWin.frame;
    printf("  content rect in screen : x %.4f  y %.4f  w %.4f  h %.4f\n", x0, y0, w0, h0);
    printf("  window.frame           : x %.1f  y %.1f  w %.1f  h %.1f\n",
           fr.origin.x, fr.origin.y, fr.size.width, fr.size.height);
    printf("  titlebar height        : %.1f  (NOT in gRect: publish_rect uses contentView)\n",
           fr.size.height - h0);
    printf("  backingScaleFactor     : %.2f  (gRect is in POINTS, like +[NSEvent mouseLocation])\n",
           gWin.backingScaleFactor);

    // ------------------------------------------------------------ A1: idempotence
    hdr("PHASE A1  publish_rect() is idempotent — the whole refutation, in one loop");
    int moved = 0;
    double wx = x0, wy = y0, ww = w0, wh = h0;   // worst observed excursion
    for (int i = 0; i < ITERS; i++) {
      publish_rect();
      if (gRectX != x0 || gRectY != y0 || gRectW != w0 || gRectH != h0) {
        moved++;
        if (fabs(gRectX - x0) > fabs(wx - x0)) wx = gRectX;
        if (fabs(gRectY - y0) > fabs(wy - y0)) wy = gRectY;
        if (fabs(gRectW - w0) > fabs(ww - w0)) ww = gRectW;
        if (fabs(gRectH - h0) > fabs(wh - h0)) wh = gRectH;
      }
    }
    printf("  %d calls; %d of them changed the rect\n", ITERS, moved);
    if (moved)
      printf("  worst excursion: dx %+.6f  dy %+.6f  dw %+.6f  dh %+.6f\n",
             wx - x0, wy - y0, ww - w0, wh - h0);
    ok("gRect is bit-identical after 20000 republishes", moved == 0);

    // ------------------------------------------------------------ A2: the transform itself
    // The exact arithmetic of gpuwin_cursor(), driven by a FIXED point standing in for a
    // stationary physical mouse. Deterministic: does not care where the real cursor is.
    hdr("PHASE A2  the transform is constant for a stationary mouse (fixed synthetic point)");
    const NSPoint m = NSMakePoint(x0 + 640.0, y0 + 415.0);   // ~centre of the content rect
    publish_rect();
    const int lx0 = (int)(m.x - gRectX), ly0 = (int)(gRectH - (m.y - gRectY));
    int drift = 0, minx = lx0, maxx = lx0, miny = ly0, maxy = ly0;
    for (int i = 0; i < ITERS; i++) {
      publish_rect();                                  // as refresh_rect_occasionally() would
      int lx = (int)(m.x - gRectX);
      int ly = (int)(gRectH - (m.y - gRectY));         // flip to top-left origin
      if (lx != lx0 || ly != ly0) drift++;
      if (lx < minx) minx = lx; if (lx > maxx) maxx = lx;
      if (ly < miny) miny = ly; if (ly > maxy) maxy = ly;
    }
    printf("  fixed mouse point      : (%.1f, %.1f) screen, bottom-left origin\n", m.x, m.y);
    printf("  transform result       : (%d, %d) window-local, top-left origin\n", lx0, ly0);
    printf("  over %d republishes : x in [%d,%d]  y in [%d,%d]  (%d differed)\n",
           ITERS, minx, maxx, miny, maxy, drift);
    ok("no drift: a stationary mouse maps to one constant point", drift == 0);
    ok("no accumulation: total excursion is 0px, not 20000px",
       maxx - minx == 0 && maxy - miny == 0);

    // ------------------------------------------------------------ A3: the live poll
    // Honest about its own flakiness: if the user moves the physical mouse mid-run the poll is
    // SUPPOSED to change, so that is a SKIP, never a FAIL.
    hdr("PHASE A3  live gpuwin_cursor() with the real mouse (skips if the mouse moves)");
    NSPoint before = [NSEvent mouseLocation];
    int c0x = 0, c0y = 0, changed = 0;
    gpuwin_cursor(&c0x, &c0y);
    for (int i = 0; i < ITERS; i++) {
      int cx = 0, cy = 0;
      publish_rect();
      gpuwin_cursor(&cx, &cy);
      if (cx != c0x || cy != c0y) changed++;
    }
    NSPoint after = [NSEvent mouseLocation];
    printf("  mouseLocation before   : (%.2f, %.2f)\n", before.x, before.y);
    printf("  mouseLocation after    : (%.2f, %.2f)\n", after.x, after.y);
    printf("  gpuwin_cursor()        : (%d, %d)   [%d of %d reads differed]\n",
           c0x, c0y, changed, ITERS);
    if (before.x != after.x || before.y != after.y) {
      printf("  SKIP — the physical mouse moved during the run; the poll is meant to follow it.\n");
    } else {
      ok("stationary physical mouse -> 20000 identical poll reads", changed == 0);
    }

    // ------------------------------------------------------------ hand off to phase B
    hdr("re-exec for PHASE B");
    printf("  setenv RDINPUT_FAKECURSOR=100,100;100,101 and exec %s\n", self);
    fflush(stdout);
    if (g_fail) {
      printf("\nPHASE A FAILED (%d) — not proceeding to phase B\n", g_fail);
      return g_fail;
    }
    setenv("RDINPUT_FAKECURSOR", "100,100;100,101", 1);
    execl(self, self, (char*)NULL);
    perror("  execl");                                 // only reached if exec failed
    return 1;
  }
}

int main(int argc, char** argv) {
  (void)argc;
  if (getenv("RDINPUT_FAKECURSOR")) {
    int rc = phase_b();
    printf("\n%s\n", rc ? "FAIL" : "ALL PASS (phases A and B)");
    return rc ? 1 : 0;
  }
  return phase_a(argv[0]) ? 1 : 0;
}
