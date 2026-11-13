// test_pointer.m — numeric trace of the POINTER COORDINATE PIPELINE.
//
// Build:
//   clang -arch arm64 -O0 -g -ObjC -o test_pointer src/test_pointer.m src/gpu_bridge.m \
//         -framework Cocoa -framework Metal -framework QuartzCore -Isrc
//
// It #includes window.m so the file-static transform helpers (ev_local_xy, publish_rect,
// gRect*, gWin) are reachable. Nothing here posts a CGEvent, so the REAL cursor is never
// moved and a concurrent GUI session is not disturbed.
//
// What it measures, all in one run:
//   1. EVENT path.  Fabricate an NSEvent whose locationInWindow is the exact window-base point
//      corresponding to a desired top-left content coordinate, push it through ev_local_xy(),
//      and print (want) vs (got). A wrong Y-flip base shows as a non-zero dy that is zero at
//      the vertical centre and maximal at top/bottom. A points-vs-pixels error shows as a
//      factor that grows with the offset.
//   2. POLL path.   Compare gpuwin_cursor() against the same transform applied by hand to
//      +[NSEvent mouseLocation] and the published content rect. Self-consistency of the poll
//      flip base (ry + rh) versus the event flip base (rh alone).
//   3. GEOMETRY.    backingScaleFactor, contentsScale, drawableSize, frame vs content rect,
//      and the titlebar height — the four numbers hypotheses 3 and 4 turn on.
//   4. POLL vs EVENT divergence, by construction: the two paths are fed the same instant and
//      their outputs compared.
#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <stdlib.h>

#include "window.m"

#define CW 800      // content width  the guest surface would ask for
#define CH 600      // content height

static int g_fail;

static void hdr(const char* s) { printf("\n===== %s =====\n", s); }

// Fabricate a mouse event addressed to gWin at window-base point p (bottom-left origin).
static NSEvent* fake_mouse(NSPoint p, NSEventType t) {
  return [NSEvent mouseEventWithType:t
                            location:p
                       modifierFlags:0
                           timestamp:[[NSProcessInfo processInfo] systemUptime]
                        windowNumber:[gWin windowNumber]
                             context:nil
                         eventNumber:0
                          clickCount:1
                            pressure:1.0];
}

int main(void) {
  @autoreleasepool {
    gpuwin_ensure_app();

    // Build the window exactly the way open_on_main() does: same style mask, same content size.
    NSRect content = NSMakeRect(0, 0, (CGFloat)CW, (CGFloat)CH);
    NSWindowStyleMask mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable;
    gWin = [[NSWindow alloc] initWithContentRect:content
                                       styleMask:mask
                                         backing:NSBackingStoreBuffered
                                           defer:NO];
    gWin.title = @"g2 pointer trace";
    gWin.releasedWhenClosed = NO;
    center_on_builtin(gWin);
    NSView* v = gWin.contentView;
    v.wantsLayer = YES;
    [gWin makeKeyAndOrderFront:nil];

    // Let AppKit settle the frame (constrainFrameRect: can shrink it), then publish.
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.30]];
    atomic_store(&gFbW, CW);
    atomic_store(&gFbH, CH);
    publish_rect();

    hdr("GEOMETRY");
    NSRect fr  = gWin.frame;
    NSRect cvs = [gWin convertRectToScreen:gWin.contentView.frame];
    printf("requested content      : %d x %d (points, == guest buffer pixels)\n", CW, CH);
    printf("window.frame           : origin (%.1f, %.1f)  size %.1f x %.1f\n",
           fr.origin.x, fr.origin.y, fr.size.width, fr.size.height);
    printf("contentView.frame      : origin (%.1f, %.1f)  size %.1f x %.1f  [window-base]\n",
           gWin.contentView.frame.origin.x, gWin.contentView.frame.origin.y,
           gWin.contentView.frame.size.width, gWin.contentView.frame.size.height);
    printf("content rect in screen : origin (%.1f, %.1f)  size %.1f x %.1f\n",
           cvs.origin.x, cvs.origin.y, cvs.size.width, cvs.size.height);
    printf("published gRect        : x %.1f  y %.1f  w %.1f  h %.1f  valid %d\n",
           gRectX, gRectY, gRectW, gRectH, gRectValid);
    printf("titlebar height        : %.1f  (frame.h - content.h)\n",
           fr.size.height - cvs.size.height);
    printf("backingScaleFactor     : %.2f   screen.backingScaleFactor %.2f\n",
           gWin.backingScaleFactor, gWin.screen.backingScaleFactor);
    printf("contentView.layer      : contentsScale %.2f\n",
           gWin.contentView.layer ? gWin.contentView.layer.contentsScale : -1.0);
    printf("screen frame           : %.0f x %.0f   visibleFrame %.0f x %.0f\n",
           gWin.screen.frame.size.width, gWin.screen.frame.size.height,
           gWin.screen.visibleFrame.size.width, gWin.screen.visibleFrame.size.height);

    if ((int)gRectW != CW || (int)gRectH != CH) {
      printf("!! content rect (%.0fx%.0f) != requested (%dx%d): AppKit constrained the window.\n",
             gRectW, gRectH, CW, CH);
      printf("!! wlsrv would still report surface %dx%d to the guest -> pointer scale is WRONG.\n",
             CW, CH);
      g_fail++;
    }

    // ---------------------------------------------------------------- 1. EVENT path
    hdr("EVENT PATH  ev_local_xy(): want (top-left content coords) vs got");
    printf("%-16s %-22s %-14s %s\n", "sample", "locationInWindow", "want (x,y)", "got (x,y)   d");
    const int xs[3] = { 0, CW / 2, CW - 1 };
    const int ys[3] = { 0, CH / 2, CH - 1 };
    const char* xn[3] = { "left", "hcentre", "right" };
    const char* yn[3] = { "top", "vcentre", "bottom" };
    for (int iy = 0; iy < 3; iy++) {
      for (int ix = 0; ix < 3; ix++) {
        int wantx = xs[ix], wanty = ys[iy];
        // Inverse of the transform under test: window-base y = contentH - topLeftY.
        NSPoint p = NSMakePoint((CGFloat)wantx, (CGFloat)(CH - wanty));
        NSEvent* e = fake_mouse(p, NSEventTypeLeftMouseDown);
        int32_t gx = 0, gy = 0;
        ev_local_xy(e, &gx, &gy);
        int dx = gx - wantx, dy = gy - wanty;
        char label[32];
        snprintf(label, sizeof label, "%s-%s", yn[iy], xn[ix]);
        printf("%-16s (%6.1f,%6.1f)       (%4d,%4d)     (%4d,%4d)  d(%+d,%+d)%s\n",
               label, p.x, p.y, wantx, wanty, gx, gy, dx, dy,
               (dx || dy) ? "   <-- MISMATCH" : "");
        if (dx || dy) g_fail++;
      }
    }

    // ---------------------------------------------------------------- 2. POLL path
    hdr("POLL PATH  gpuwin_cursor() vs the same flip done by hand");
    NSPoint m = [NSEvent mouseLocation];
    int px = 0, py = 0;
    gpuwin_cursor(&px, &py);
    double hx = m.x - gRectX;
    double hy = gRectH - (m.y - gRectY);
    printf("live +[NSEvent mouseLocation] : (%.1f, %.1f)  [screen, bottom-left origin]\n", m.x, m.y);
    printf("gpuwin_cursor()               : (%d, %d)\n", px, py);
    printf("hand transform                : (%.1f, %.1f)\n", hx, hy);
    printf("delta                         : (%+.1f, %+.1f)%s\n",
           px - hx, py - hy, (fabs(px - hx) > 1 || fabs(py - hy) > 1) ? "  <-- MISMATCH" : "");

    // Cross-check the poll flip base against the event flip base at the SAME physical point.
    // Feed the event path the window-base location that corresponds to the live cursor.
    NSPoint wb = [gWin convertPointFromScreen:m];
    NSEvent* e2 = fake_mouse(wb, NSEventTypeMouseMoved);
    int32_t ex = 0, ey = 0;
    ev_local_xy(e2, &ex, &ey);
    printf("\nsame physical point through BOTH paths:\n");
    printf("  poll  gpuwin_cursor()  -> (%d, %d)\n", px, py);
    printf("  event ev_local_xy()    -> (%d, %d)   [locationInWindow (%.1f, %.1f)]\n",
           ex, ey, wb.x, wb.y);
    printf("  agreement              : %s (d %+d, %+d)\n",
           (abs(ex - px) <= 1 && abs(ey - py) <= 1) ? "YES" : "NO",
           ex - px, ey - py);

    // ---------------------------------------------------------------- 3. no-window fallback
    hdr("EVENT PATH, no-window fallback branch ([e window] == nil)");
    // windowNumber 0 -> [e window] is nil. -[NSEvent locationInWindow] is then in SCREEN
    // coordinates, so the event still carries its own position and the fallback must use it.
    // Aim at content point (100, 250) and check the fallback lands there rather than on the
    // live cursor (which is somewhere else entirely).
    const int fwantx = 100, fwanty = 250;
    NSPoint scr = NSMakePoint(gRectX + fwantx, gRectY + (gRectH - fwanty));
    NSEvent* e3 = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                     location:scr
                                modifierFlags:0
                                    timestamp:[[NSProcessInfo processInfo] systemUptime]
                                 windowNumber:0
                                      context:nil
                                  eventNumber:0
                                   clickCount:1
                                     pressure:1.0];
    int32_t fx = 0, fy = 0;
    ev_local_xy(e3, &fx, &fy);
    printf("event screen location (%.1f, %.1f), windowNumber 0\n", scr.x, scr.y);
    printf("  want (event's own position) : (%d, %d)\n", fwantx, fwanty);
    printf("  got                         : (%d, %d)\n", fx, fy);
    printf("  live cursor (the wrong answer): (%d, %d)\n", px, py);
    if (fx == fwantx && fy == fwanty) {
      printf("  => fallback uses the EVENT position. OK\n");
    } else {
      printf("  => MISMATCH: fallback is not honouring the event's own position\n");
      g_fail++;
    }

    hdr(g_fail ? "RESULT: MISMATCHES FOUND" : "RESULT: transforms agree");
    printf("mismatches: %d\n", g_fail);
    return g_fail ? 1 : 0;
  }
}
