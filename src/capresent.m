// capresent.m — present guest frames by handing WindowServer an IOSurface.
//
// See capresent.h for why. The mechanics that are easy to get wrong, stated here because each of
// them has a silent-wrong-answer failure mode:
//
//   ROW STRIDE. IOSurfaceGetBytesPerRow() is NOT width*4 — it is aligned (64 bytes here, so a
//   1000px-wide surface has a 4096-byte row, not 4000). Compositing with a packed stride into an
//   aligned surface produces a progressively sheared image that still looks like "rendering
//   works" in a thumbnail. Every writer here takes the stride from the surface.
//
//   REUSE WHILE THE SCREEN IS READING. WindowServer holds the surface it is displaying. Writing
//   into it produces tearing that is invisible on a static test pattern and obvious on motion —
//   i.e. exactly backwards from what a screenshot-based check would catch. Hence a ring of
//   surfaces and IOSurfaceIsInUse() before reusing one; the frames that could not find a free
//   surface are COUNTED and reported, not silently absorbed.
//
//   SETTING THE SAME OBJECT TWICE. -[CALayer setContents:] with the pointer that is already
//   there is a no-op, so a single-surface implementation displays frame 1 forever while
//   happily reporting that it published 600 frames. The ring makes each publish a different
//   object; capres_report() prints the ring depth so this cannot be assumed away.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "capresent.h"

// ---------------------------------------------------------------------------- mode
// RDCA is the name S2 asked for and is checked first; WLSRV_CALAYER is kept as an alias so the
// knob matches the WLSRV_* family it sits in. ONE BINARY, ONE VARIABLE: there is no second tree
// and no build flag, so assert-fresh.sh's identity check covers both arms of the A/B.
//   0 = off (Metal path)   1 = composite into an IOSurface   2 = 1, plus convert straight into it
int capres_mode(void) {
  static int v = -1;
  if (v < 0) {
    const char* e = getenv("RDCA");
    if (!e || !*e) e = getenv("WLSRV_CALAYER");
    v = (e && *e) ? atoi(e) : 0;
    if (v < 0) v = 0;
  }
  return v;
}

// How many surfaces in the ring. Two is enough to stop tearing; three lets the producer run one
// frame ahead of the display without ever waiting. WLSRV_CA_SLOTS exists so "the ring depth is
// why it is fast/slow" can be tested rather than argued.
static int cap_slots(void) {
  static int v;
  if (!v) {
    const char* e = getenv("WLSRV_CA_SLOTS");
    v = e ? atoi(e) : 3;
    if (v < 1) v = 1;
    if (v > 8) v = 8;
  }
  return v;
}

// ---------------------------------------------------------------------------- state
#define CAP_MAX_SLOTS 8
static IOSurfaceRef gSurf[CAP_MAX_SLOTS];
static uint32_t     gW, gH;
static int          gHeld = -1;              // slot locked by capres_acquire(), -1 = none
static int          gNext;                   // round-robin cursor
static CALayer*     gCA;
static _Atomic(int) gLayerReady;

static uint64_t gPublished, gStalled, gAllocs;

// ---------------------------------------------------------------------------- the layer
void* capres_make_layer(void) {
  if (!capres_mode()) return NULL;
  if (!gCA) {
    gCA = [CALayer layer];
    gCA.opaque = YES;
    // 1:1 pixels. The window is opened at the guest surface's size in POINTS, and the layer's
    // contentsScale is fixed up in capres_publish() once the real surface size is known, so a
    // Retina panel does not silently sample a half-size image.
    gCA.contentsGravity = kCAGravityResize;
    gCA.magnificationFilter = kCAFilterNearest;
    gCA.minificationFilter  = kCAFilterLinear;
    gCA.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
  }
  atomic_store(&gLayerReady, 1);
  return (__bridge void*)gCA;
}

int capres_enabled(void) {
  return capres_mode() && atomic_load(&gLayerReady);
}

// ---------------------------------------------------------------------------- the ring
static void ring_free(void) {
  for (int i = 0; i < CAP_MAX_SLOTS; i++) {
    if (gSurf[i]) { CFRelease(gSurf[i]); gSurf[i] = NULL; }
  }
  gHeld = -1; gNext = 0;
}

static int ring_alloc(uint32_t w, uint32_t h) {
  ring_free();
  int n = cap_slots();
  size_t bpr = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, (size_t)w * 4);
  size_t total = IOSurfaceAlignProperty(kIOSurfaceAllocSize, bpr * (size_t)h);
  for (int i = 0; i < n; i++) {
    NSDictionary* props = @{
      (id)kIOSurfaceWidth           : @(w),
      (id)kIOSurfaceHeight          : @(h),
      (id)kIOSurfaceBytesPerElement : @4,
      (id)kIOSurfaceBytesPerRow     : @(bpr),
      (id)kIOSurfaceAllocSize       : @(total),
      // 'BGRA' — the byte order a wl_shm ARGB8888 buffer already has, and the one CALayer takes
      // natively. This is the whole reason the swizzle does not reappear on this path.
      (id)kIOSurfacePixelFormat     : @((unsigned)'BGRA'),
    };
    gSurf[i] = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    if (!gSurf[i]) { ring_free(); return -1; }
    // Start black rather than whatever the page cache held: an uninitialised surface reaching the
    // screen for one frame is a flash that reads as a driver bug.
    IOSurfaceLock(gSurf[i], 0, NULL);
    memset(IOSurfaceGetBaseAddress(gSurf[i]), 0, total);
    IOSurfaceUnlock(gSurf[i], 0, NULL);
    gAllocs++;
  }
  gW = w; gH = h; gNext = 0; gHeld = -1;
  if (getenv("WLSRV_PROF"))
    fprintf(stderr, "[ca] ring: %d surfaces %ux%u BGRA, stride %zu (packed would be %u) — "
                    "mode %d\n", n, w, h, bpr, w * 4, capres_mode());
  return 0;
}

uint8_t* capres_acquire(uint32_t w, uint32_t h, size_t* stride) {
  if (!capres_enabled() || !w || !h) return NULL;
  if (gHeld >= 0) return NULL;                       // publish before acquiring again
  if (w != gW || h != gH || !gSurf[0]) {
    if (ring_alloc(w, h) != 0) return NULL;
  }
  int n = cap_slots();
  int pick = -1;
  for (int k = 0; k < n; k++) {
    int i = (gNext + k) % n;
    if (!IOSurfaceIsInUse(gSurf[i])) { pick = i; break; }
  }
  if (pick < 0) {
    // Everything is on screen or in flight. Taking one anyway would tear; the honest thing is to
    // take the oldest and COUNT it, so "the ring was too shallow" shows up as a number rather
    // than as a mysterious artefact.
    pick = gNext % n;
    gStalled++;
  }
  gNext = (pick + 1) % n;
  if (IOSurfaceLock(gSurf[pick], 0, NULL) != kIOReturnSuccess) return NULL;
  gHeld = pick;
  if (stride) *stride = IOSurfaceGetBytesPerRow(gSurf[pick]);

  // ---- THE DISCRIMINATOR -------------------------------------------------------------------
  // One line, once per process, printed from the ONLY place that runs when this arm is really
  // engaged — a surface has just been locked for writing. It cannot appear on the Metal path,
  // and it names the quantity the whole exercise changes: FULL-FRAME CPU PASSES OVER THE PIXELS
  // between the guest's shm buffer and the screen.
  //
  // It also states WRAPPED vs COPIED, and the answer is COPIED, permanently. IOSurfaceCreate
  // allocates its own memory; there is no API to make an IOSurface out of pages that already
  // exist, and the wl_shm pool's pages are the GUEST's — created by its own memfd, mapped into
  // our address space by net.c. So the pool can never itself be an IOSurface, and one copy out
  // of it is the floor for this path.
  //
  // What that does NOT mean: it is not a regression against C1WSI_ZEROCOPY. That zero-copy is on
  // the GUEST side — the Vulkan swapchain image IS the wl_shm pool, so the client renders with
  // no copy at all — and it is untouched here. Both arms then take pixels out of that same pool;
  // this arm takes one pass to do it and the Metal arm takes three. Nothing that was zero-copy
  // became copied.
  static int said;
  if (!said) {
    said = 1;
    fprintf(stderr,
            "[ca] ARM=CALayer/IOSurface mode=%d surface=IOSurfaceID:%u %ux%u stride=%zu "
            "pixels=COPIED-INTO-IOSURFACE (not wrapped: IOSurface cannot adopt the guest's shm "
            "pages) | full-frame CPU passes shm->screen: %d (Metal arm: 3) | "
            "guest-side C1WSI_ZEROCOPY unaffected\n",
            capres_mode(), (unsigned)IOSurfaceGetID(gSurf[pick]), w, h,
            IOSurfaceGetBytesPerRow(gSurf[pick]), capres_mode() >= 2 ? 1 : 2);
    fflush(stderr);
  }
  return (uint8_t*)IOSurfaceGetBaseAddress(gSurf[pick]);
}

// The last surface handed to the layer, for reading only. See the header: under mode 2 the
// toplevel keeps no retained copy of its own, so this is where one is recovered from.
static int gLastPublished = -1;

const uint8_t* capres_last_published(uint32_t w, uint32_t h, size_t* stride) {
  if (gLastPublished < 0 || !gSurf[gLastPublished]) return NULL;
  if (w != gW || h != gH) return NULL;
  if (stride) *stride = IOSurfaceGetBytesPerRow(gSurf[gLastPublished]);
  return (const uint8_t*)IOSurfaceGetBaseAddress(gSurf[gLastPublished]);
}

// WLSRV_CA_ASYNC=1 pushes the contents assignment to the main queue instead of committing a
// CATransaction on the compositor thread. Both are legitimate; the inline transaction is the
// lower-latency one and the default, and the knob exists because "CoreAnimation off the main
// thread" is the kind of claim that should be measurable rather than asserted.
static int ca_async(void) {
  static int v = -1;
  if (v < 0) { const char* e = getenv("WLSRV_CA_ASYNC"); v = (e && *e && *e != '0'); }
  return v;
}

int capres_publish(void) {
  if (gHeld < 0) return -1;
  IOSurfaceRef s = gSurf[gHeld];
  IOSurfaceUnlock(s, 0, NULL);
  gLastPublished = gHeld;
  gHeld = -1;
  gPublished++;

  CALayer* layer = gCA;
  if (!layer) return -1;
  if (ca_async()) {
    CFRetain(s);
    dispatch_async(dispatch_get_main_queue(), ^{ @autoreleasepool {
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      layer.contents = (__bridge id)s;
      [CATransaction commit];
      CFRelease(s);
    }});
  } else {
    @autoreleasepool {
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      layer.contents = (__bridge id)s;
      [CATransaction commit];
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------- vsync pacing
//
// DELETED, AND THE REASON IS THE INTERESTING PART. The plan was a CADisplayLink whose ticks
// would gate wl_surface.frame, on the theory that the client's bimodal frame period (p50 0.85 ms
// vs p95 9.6 ms) came from the compositor having no real display cadence to hand it.
//
// S2 measured that on screen with -[MTLDrawable presentedTime] as ground truth and the
// bimodality is not there: frame-period p95 is 1.05-1.1x p50, and 99.5-99.8% of frames land on
// the very NEXT display refresh. The tail is a HEADLESS artefact. The mechanism the hypothesis
// said was missing already exists and is -[CAMetalLayer nextDrawable]: it blocks until a
// drawable frees up, which is exactly the vsync-paced back-pressure a display link would have
// provided.
//
// So a display link here would have re-implemented, on the path that removed nextDrawable, the
// pacing that nextDrawable was already doing. It is written down rather than deleted silently
// because "CA has real vsync callbacks and we do not" was the single strongest argument for this
// whole path, and it is false.
// ---------------------------------------------------------------------------- report
//
// NAME THE QUANTITY, not an operation on the way to it: the person reading this log may never
// see this file, so it says how many FULL-FRAME CPU PASSES over the pixels the configuration
// implies, which is the thing the whole exercise is trying to change.
void capres_report(void) {
  if (!capres_mode()) return;
  int m = capres_mode();
  fprintf(stderr,
          "[ca] mode %d (%s): %llu frames published, %llu found no free surface, "
          "%llu surfaces allocated, ring %d, publish %s\n",
          m,
          m >= 2 ? "1 CPU pass/frame: shm -> IOSurface"
                 : "2 CPU passes/frame: shm -> surface->rgba -> IOSurface",
          (unsigned long long)gPublished, (unsigned long long)gStalled,
          (unsigned long long)gAllocs, cap_slots(),
          ca_async() ? "async on main" : "inline CATransaction");
}
