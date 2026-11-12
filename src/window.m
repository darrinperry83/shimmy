// window.m — the Metal-backed NSWindow the guest's GUI is presented into.
//
// Owns everything Cocoa: the NSWindow, its CAMetalLayer, the blit pipeline, the staging
// texture, and the gpuwin_* entry points declared in gpu.h.
//
// THREADING. Cocoa is main-thread-only, and the guest/X-server run on worker threads, so
// anything Cocoa funnels through run_on_main(). Under the real shim the main thread sits inside
// -[NSApplication run], which drains the main dispatch queue, so dispatch_sync from a worker
// completes normally. The one rule: never call run_on_main() from inside a block that is
// already executing on main (dispatch_sync to the queue you are on = instant deadlock), so the
// internal helpers below are all "must already be on main" and never re-marshal.
//
// Presenting a FRAME, however, deliberately does NOT hop to main. Both present paths do their
// Metal work on the calling thread and hop to main only for Cocoa state that actually changed
// (window creation, drawableSize, layer properties). A dispatch_sync per frame serialized the
// producer against everything else the run loop was doing, and put the wait for a drawable —
// i.e. the wait for a display refresh — on the UI thread; it measured 18.7 ms of a 20 ms frame.
//
// CURSOR. -[NSApplication run] never calls gpuwin_poll(), so gpuwin_cursor() cannot be fed
// from poll. Instead the window's content rect is cached in *screen* coordinates on the main
// thread during gpuwin_present() (which the X server calls every frame anyway), and
// gpuwin_cursor() computes live from +[NSEvent mouseLocation] — documented thread-safe —
// minus that cached origin, flipped to X11's top-left origin and clamped to the content rect.
//
// TWO PRESENT PATHS, one shared tail (present_source()).
//   gpuwin_present(rgba)        — ordinary X windows. The frame is a CPU framebuffer, so it
//                                 must be uploaded; three staging textures are cycled under a
//                                 semaphore released by the command buffer's completion
//                                 handler, so the upload for frame N+1 can never land on a
//                                 texture the GPU is still sampling for frame N. Frames arrive
//                                 rarely and each one is unique, so this path WAITS for a slot.
//   gpuwin_present_texture(tex) — the GL fast path. The frame is ALREADY an MTLTexture that
//                                 glbridge just rendered, so nothing is copied through the CPU
//                                 at all: blit texture->drawable and present. The producer runs
//                                 far faster than the display, so this path never waits — a
//                                 frame that arrives while another is still in flight is simply
//                                 dropped, and the newest one wins.
//
// PACING. A present slot is handed back by the drawable's *presented* handler, not by the
// command buffer's completion handler, so an attempt can only succeed once the previous frame
// has really been shown. That is what makes presents land exactly one refresh apart (measured
// p50 8.33 ms, p95 8.34, 100% of frames one refresh apart on a 120 Hz panel) without anyone
// blocking. See the long note in x11srv.c's SwapBuffers for why blocking here — the obvious
// design — turns metronomic delivery into visible stutter.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreGraphics/CoreGraphics.h>
#include <pthread.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread/qos.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gpu.h"
#include "capresent.h"
#include "present_feedback.h"
#include "rdgpu_scanout_client.h"
#include <IOKit/pwr_mgt/IOPMLib.h>

@interface RDPrimaryGuestWindow : NSWindow
@end
@implementation RDPrimaryGuestWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

// Provided by gpu_bridge.m (lazily creates the device/queue; safe from any thread).
id<MTLDevice>       gpu_metal_device(void);
id<MTLCommandQueue> gpu_metal_queue(void);

// ---------------------------------------------------------------- main-thread marshalling
static void run_on_main(dispatch_block_t b) {
  if ([NSThread isMainThread]) b();
  else dispatch_sync(dispatch_get_main_queue(), b);
}

// ---------------------------------------------------------------- kiosk takeover
// RDNATIVE_KIOSK=1 raises the full-screen borderless desktop window ABOVE the menu
// bar, Dock and desktop-icon levels, so the Linux desktop owns every pixel of the
// display while WindowServer keeps compositing. This is the pragmatic "take over the
// screen" fallback: no CGDisplayCapture, so nothing is left black if the process dies.
static int kiosk_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("RDNATIVE_KIOSK") != NULL;
  return enabled;
}
static NSWindowLevel kiosk_window_level(void) {
  // Above the Dock (kCGDockWindowLevelKey) and the menu bar / status strip
  // (kCGStatusWindowLevelKey). Desktop icons and the wallpaper live far below.
  return kiosk_enabled() ? CGWindowLevelForKey(kCGStatusWindowLevelKey) + 1 : NSNormalWindowLevel;
}

// RDNATIVE_SCREENSAVER_SUPPRESS=1 takes IOPMAssertions that stop the idle timer from
// firing the screensaver / display sleep. Assertions are process-scoped: killing the
// shim releases them automatically, and the explicit release in kiosk_teardown() (run
// via atexit) restores them on a clean exit.
static IOPMAssertionID gPMDisplaySleep;
static IOPMAssertionID gPMSystemSleep;
static int gPMAssertionActive;
static int screensaver_suppress_enabled(void) {
  static int v = -1;
  if (v < 0) { const char* e = getenv("RDNATIVE_SCREENSAVER_SUPPRESS"); v = (e && *e && *e != '0'); }
  return v;
}
static void kiosk_teardown(void) {
  if (gPMDisplaySleep) { IOPMAssertionRelease(gPMDisplaySleep); gPMDisplaySleep = 0; }
  if (gPMSystemSleep)   { IOPMAssertionRelease(gPMSystemSleep);   gPMSystemSleep = 0; }
  if (gPMAssertionActive) {
    gPMAssertionActive = 0;
    fprintf(stderr, "[kiosk] screen-saver assertions released; macOS idle policy restored\n");
  }
}
static void kiosk_suppress_screensaver(void) {
  if (!screensaver_suppress_enabled() || gPMAssertionActive) return;
  IOReturn r1 = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
      kIOPMAssertionLevelOn, CFSTR("Hyprland kiosk takeover"), &gPMDisplaySleep);
  IOReturn r2 = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
      kIOPMAssertionLevelOn, CFSTR("Hyprland kiosk takeover"), &gPMSystemSleep);
  if (r1 != kIOReturnSuccess || r2 != kIOReturnSuccess) {
    if (gPMDisplaySleep) { IOPMAssertionRelease(gPMDisplaySleep); gPMDisplaySleep = 0; }
    if (gPMSystemSleep)   { IOPMAssertionRelease(gPMSystemSleep);   gPMSystemSleep = 0; }
    fprintf(stderr, "[kiosk] screen-saver assertion FAILED (0x%x, 0x%x); screensaver not suppressed\n",
            r1, r2);
    return;
  }
  gPMAssertionActive = 1;
  atexit(kiosk_teardown);
  fprintf(stderr, "[kiosk] screen-saver suppressed (IOPMAssertion display=%u system=%u)\n",
          (unsigned)gPMDisplaySleep, (unsigned)gPMSystemSleep);
}

// ---------------------------------------------------------------- state
// Touched only on the main thread unless noted.
static NSWindow*                  gWin;
static NSMutableArray<NSWindow*>* gDesktopBackgrounds;
static NSMutableArray<NSWindow*>* gMenuMasks;
static NSMutableArray<NSWindow*>* gDockMasks;
static NSWindow*                  gExtraCropWin;
static CAMetalLayer*              gExtraCropLayer;
static _Atomic(int)               gExtraCropVisible;
static NSWindow*                  gNativeChild;
static NSString*                  gWantedTitle;
static void ev_install_monitor(void);   // input capture, defined further down
static void ev_selftest_arm(void);
static void ev_selftest_mouse_arm(void);   // scripted pointer self-test, defined further down
static CAMetalLayer*              gLayer;
// Non-nil ONLY under WLSRV_CALAYER: the plain CALayer that takes an IOSurface as its contents,
// installed as the content view's layer INSTEAD of the CAMetalLayer. Instead, not as well —
// leaving a live CAMetalLayer in the tree would hand WindowServer a second layer to composite
// and quietly tax the very path being measured.
static CALayer*                   gCAPresLayer;
static id<MTLRenderPipelineState> gPipe;
static id<MTLSamplerState>        gSampler;
static id                         gDelegate;   // strong ref: NSWindow.delegate is weak
static int                        gSettingContentSize;
static _Atomic(int)               gPendingResizeW, gPendingResizeH;

// Drawable size as last published by main, so the GL path can check it without touching the
// layer from another thread.
static _Atomic(unsigned long)     gDrawableW, gDrawableH;
static atomic_int                  gDirectGPUActive;
int gpuwin_direct_active(void) { return atomic_load(&gDirectGPUActive); }
void gpuwin_direct_active_set(int active) { atomic_store(&gDirectGPUActive, active != 0); }
static atomic_uint                 gSplitWidth;
static atomic_ullong               gSplitLastCheckNS;
static atomic_int                   gHostFocusTarget; // 0 unset, 1 primary, 2 extra, 3 native
static int                          gAppliedHostFocus;
struct RDHostCrop { unsigned x, y, w, h; };
struct RDRegionSpec { uint64_t id; struct RDHostCrop crop; int panel; };
@interface RDRegionFrame : NSObject
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) CAMetalLayer* layer;
@property(nonatomic) uint64_t ident;
@property(nonatomic) struct RDHostCrop crop;
@property(nonatomic) BOOL panel;
@property(nonatomic) NSRect screenFrame;
@property(nonatomic) CGFloat scale;   // backing scale of the window's screen (points -> pixels)
@end
@implementation RDRegionFrame
@end
static NSMutableDictionary<NSNumber*, RDRegionFrame*>* gRegionsByID; // main thread
static NSArray<RDRegionFrame*>* gRegionFrames; // immutable, protected by gRegionLock
static struct RDRegionSpec*       gDesiredRegions;
static size_t                     gDesiredRegionCount;
static pthread_mutex_t            gRegionLock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int                 gRegionsDirty;
static NSUInteger                 gRegionOutputW, gRegionOutputH; // main thread
static atomic_ullong               gRegionLastCheckNS;
static atomic_int                  gRegionFileEvent = 1;
static atomic_int                  gRegionWatchActive;
static dispatch_source_t           gRegionDirectoryWatch;
static dispatch_source_t           gFocusDirectoryWatch;
static atomic_ullong               gDynamicFocusID;
static atomic_int                  gDynamicFocusMac;
static uint64_t                    gAppliedDynamicFocusID;
static int                         gAppliedDynamicFocusMac;
static struct RDHostCrop           gHostCrop;
static struct RDHostCrop           gExtraCrop;
static pthread_mutex_t            gHostCropLk = PTHREAD_MUTEX_INITIALIZER;
static CGFloat                    gFullOriginX, gFullOriginY;
static double                     gExtraRectX, gExtraRectY, gExtraRectW, gExtraRectH;
// Fitted full-screen display geometry, published on the MAIN thread by size_layer()
// and re-published on every hotplug re-fit. Read only from main (size_layer and
// ensure_ready_on_main), never from the presenter thread.
static NSUInteger                 gFitDisplayW, gFitDisplayH;
static CGFloat                    gFitDisplayScale;
static int split_output_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("RDNATIVE_SPLIT_OUTPUT") && !getenv("WLSRV_CALAYER");
  return enabled;
}
static int fit_display_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("RDNATIVE_FIT_DISPLAY") != NULL;
  return enabled;
}
static int dynamic_regions_enabled(void) {
  return split_output_enabled() && getenv("RDNATIVE_REGION_LIST_PATH") != NULL;
}
// The coordinator atomically renames both publications, so watch their containing
// directories rather than an inode that disappears on every update. Vnode events
// only mark data dirty; the render thread reads and validates it on its next frame.
static dispatch_source_t watch_region_directory(NSString* path) {
  NSString* directory = [path stringByDeletingLastPathComponent];
  int fd = open(directory.fileSystemRepresentation, O_EVTONLY);
  if (fd < 0) return nil;
  dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, (uintptr_t)fd,
      DISPATCH_VNODE_WRITE | DISPATCH_VNODE_RENAME | DISPATCH_VNODE_DELETE,
      dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0));
  if (!source) { close(fd); return nil; }
  dispatch_source_set_event_handler(source, ^{ atomic_store(&gRegionFileEvent, 1); });
  dispatch_source_set_cancel_handler(source, ^{ close(fd); });
  dispatch_resume(source);
  return source;
}
static void start_dynamic_region_watchers(void) { // main thread, once
  if (!dynamic_regions_enabled() || gRegionDirectoryWatch) return;
  NSString* regionPath = @(getenv("RDNATIVE_REGION_LIST_PATH"));
  gRegionDirectoryWatch = watch_region_directory(regionPath);
  const char* focus = getenv("RDNATIVE_REGION_FOCUS_PATH");
  if (focus) {
    NSString* focusPath = @(focus);
    if (![[focusPath stringByDeletingLastPathComponent]
            isEqualToString:[regionPath stringByDeletingLastPathComponent]])
      gFocusDirectoryWatch = watch_region_directory(focusPath);
  }
  atomic_store(&gRegionWatchActive, gRegionDirectoryWatch != nil &&
      (!focus || [[@(focus) stringByDeletingLastPathComponent]
                    isEqualToString:[regionPath stringByDeletingLastPathComponent]] ||
       gFocusDirectoryWatch != nil));
  atomic_store(&gRegionFileEvent, 1);
  fprintf(stderr, "[native-region] directory event watch %s\n",
          atomic_load(&gRegionWatchActive) ? "active" : "unavailable; 50 ms fallback");
}
static NSArray<RDRegionFrame*>* region_frames_snapshot(void) {
  pthread_mutex_lock(&gRegionLock);
  NSArray<RDRegionFrame*>* snapshot = gRegionFrames;
  pthread_mutex_unlock(&gRegionLock);
  return snapshot;
}
static void refresh_dynamic_regions(NSUInteger guestW, NSUInteger guestH) {
  if (!dynamic_regions_enabled()) return;
  unsigned long long now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
  unsigned long long last = atomic_load(&gRegionLastCheckNS);
  unsigned long long fallback = atomic_load(&gRegionWatchActive) ? 2000000000ULL : 50000000ULL;
  if (!atomic_load(&gRegionFileEvent) && now - last < fallback) return;
  if (
      !atomic_compare_exchange_strong(&gRegionLastCheckNS, &last, now)) return;
  atomic_store(&gRegionFileEvent, 0);
  const char* path = getenv("RDNATIVE_REGION_LIST_PATH");
  struct RDRegionSpec* next = NULL;
  size_t count = 0, capacity = 0;
  int allocationFailed = 0;
  FILE* f = fopen(path, "r");
  if (f) {
    char line[128];
    while (fgets(line, sizeof line, f)) {
      unsigned long long ident;
      unsigned x, y, w, h;
      char role[16] = {0};
      int fields = sscanf(line, "%llx %u %u %u %u %15s", &ident, &x, &y, &w, &h, role);
      if ((fields != 5 && fields != 6) ||
          (fields == 6 && strcmp(role, "panel") != 0 && strcmp(role, "popup") != 0 &&
           strcmp(role, "background") != 0) ||
          !ident || !w || !h || x >= guestW || y >= guestH ||
          w > guestW - x || h > guestH - y) continue;
      int duplicate = 0;
      for (size_t i = 0; i < count; i++) if (next[i].id == ident) { duplicate = 1; break; }
      if (duplicate) continue;
      if (count == capacity) {
        size_t newcap = capacity ? capacity * 2 : 8;
        struct RDRegionSpec* grown = realloc(next, newcap * sizeof *next);
        if (!grown) { allocationFailed = 1; break; }
        next = grown; capacity = newcap;
      }
      memset(&next[count], 0, sizeof next[count]);
      next[count].id = ident;
      next[count].crop = (struct RDHostCrop){x, y, w, h};
      next[count].panel = fields == 6 && strcmp(role, "background") != 0 ? 1 :
                          fields == 6 ? -1 : 0;
      count++;
    }
    fclose(f);
  }
  if (allocationFailed) { free(next); return; }
  pthread_mutex_lock(&gRegionLock);
  int changed = count != gDesiredRegionCount ||
      (count && memcmp(next, gDesiredRegions, count * sizeof *next) != 0);
  if (changed) {
    free(gDesiredRegions);
    gDesiredRegions = next;
    gDesiredRegionCount = count;
    atomic_store(&gRegionsDirty, 1);
    atomic_store(&gDrawableW, 0);
  }
  pthread_mutex_unlock(&gRegionLock);
  if (!changed) free(next);
  const char* focusPath = getenv("RDNATIVE_REGION_FOCUS_PATH");
  if (focusPath) {
    char target[64] = {0};
    FILE* ff = fopen(focusPath, "r");
    if (ff) { (void)fgets(target, sizeof target, ff); fclose(ff); }
    int mac = strcmp(target, "mac\n") == 0 || strcmp(target, "mac") == 0;
    unsigned long long id = mac ? 0 : strtoull(target, NULL, 0);
    int neutral = strcmp(target, "none\n") == 0 || strcmp(target, "none") == 0;
    if ((mac || id || neutral) && (mac != atomic_load(&gDynamicFocusMac) ||
                        id != atomic_load(&gDynamicFocusID))) {
      atomic_store(&gDynamicFocusMac, mac);
      atomic_store(&gDynamicFocusID, id);
      atomic_store(&gDrawableW, 0);
    }
  }
}
static NSUInteger host_output_width(NSUInteger guestW) {
  if (!split_output_enabled() || guestW < 2) return guestW;
  pthread_mutex_lock(&gHostCropLk);
  unsigned cropW = gHostCrop.w;
  pthread_mutex_unlock(&gHostCropLk);
  if (cropW > 0 && cropW <= guestW) return cropW;
  unsigned dynamic = atomic_load(&gSplitWidth);
  return dynamic > 0 && dynamic <= guestW ? dynamic : (guestW + 1) / 2;
}
static NSUInteger host_output_height(NSUInteger guestH) {
  if (!split_output_enabled()) return guestH;
  pthread_mutex_lock(&gHostCropLk);
  unsigned cropH = gHostCrop.h;
  pthread_mutex_unlock(&gHostCropLk);
  return cropH > 0 && cropH <= guestH ? cropH : guestH;
}
static struct RDHostCrop host_crop_snapshot(NSUInteger guestW, NSUInteger guestH) {
  struct RDHostCrop c = {0, 0, (unsigned)guestW, (unsigned)guestH};
  if (!split_output_enabled()) return c;
  // Full-screen fit: the primary window shows the WHOLE guest framebuffer, scaled
  // by the drawable size, never a horizontal slice. The split-width crop logic
  // below is for the side-by-side "split output" research mode only.
  if (fit_display_enabled()) return c;
  pthread_mutex_lock(&gHostCropLk);
  struct RDHostCrop configured = gHostCrop;
  pthread_mutex_unlock(&gHostCropLk);
  if (configured.w && configured.h && configured.x < guestW && configured.y < guestH &&
      configured.w <= guestW - configured.x && configured.h <= guestH - configured.y) return configured;
  c.w = (unsigned)host_output_width(guestW);
  return c;
}
// The primary layer's drawable size for a given guest framebuffer, on the MAIN
// thread. In full-screen fit mode this is the fitted display's PHYSICAL size
// (points x backingScaleFactor), so a re-fit onto a differently-sized (or Retina)
// display scales the whole desktop instead of cropping it.
static CGSize expected_drawable_size(NSUInteger guestW, NSUInteger guestH) {
  if (split_output_enabled() && fit_display_enabled() && gFitDisplayW && gFitDisplayH) {
    CGFloat scale = gFitDisplayScale > 0.5 ? gFitDisplayScale : 1.0;
    return CGSizeMake((CGFloat)gFitDisplayW * scale, (CGFloat)gFitDisplayH * scale);
  }
  return CGSizeMake((CGFloat)host_output_width(guestW), (CGFloat)host_output_height(guestH));
}
static struct RDHostCrop extra_crop_snapshot(NSUInteger guestW, NSUInteger guestH) {
  struct RDHostCrop empty = {0};
  pthread_mutex_lock(&gHostCropLk);
  struct RDHostCrop c = gExtraCrop;
  pthread_mutex_unlock(&gHostCropLk);
  if (!c.w || !c.h || c.x >= guestW || c.y >= guestH ||
      c.w > guestW - c.x || c.h > guestH - c.y) return empty;
  return c;
}
static void refresh_split_width(NSUInteger guestW, NSUInteger guestH) {
  if (!split_output_enabled()) return;
  const char* cropPath = getenv("RDNATIVE_CROP_PATH");
  const char* path = cropPath ?: getenv("RDNATIVE_SPLIT_WIDTH_PATH");
  const char* extraPath = getenv("RDNATIVE_EXTRA_CROP_PATH");
  const char* focusPath = getenv("RDNATIVE_HOST_FOCUS_PATH");
  if (!path && !extraPath && !focusPath) return;
  unsigned long long now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
  unsigned long long last = atomic_load(&gSplitLastCheckNS);
  if (now - last < 50000000ULL ||
      !atomic_compare_exchange_strong(&gSplitLastCheckNS, &last, now)) return;
  unsigned x = 0, y = 0, width = 0, height = (unsigned)guestH;
  int valid = 0;
  FILE* f = path ? fopen(path, "r") : NULL;
  if (f) {
    valid = cropPath ? fscanf(f, "%u %u %u %u", &x, &y, &width, &height) == 4 :
                       fscanf(f, "%u", &width) == 1;
    fclose(f);
  } else if (cropPath && getenv("RDNATIVE_REGIONS_DYNAMIC")) {
    // A closed primary Linux toplevel leaves its stable host slot parked at one pixel.
    // Other region windows can continue rendering while the slot waits for a new ID.
    x = y = 0; width = height = 1; valid = 1;
  }
  if (valid && cropPath && width > 0 && height > 0 && x < guestW && y < guestH &&
      width <= guestW - x && height <= guestH - y) {
    struct RDHostCrop next = {x, y, width, height};
    pthread_mutex_lock(&gHostCropLk);
    int changed = memcmp(&gHostCrop, &next, sizeof next) != 0;
    if (changed) gHostCrop = next;
    pthread_mutex_unlock(&gHostCropLk);
    if (changed) {
      atomic_store(&gDrawableW, 0);
      fprintf(stderr, "[native-crop] guest rect %u,%u %ux%u\n", x, y, width, height);
    }
  } else if (valid && !cropPath && width > 0 && width <= guestW && width != atomic_load(&gSplitWidth)) {
    atomic_store(&gSplitWidth, width);
    atomic_store(&gDrawableW, 0); // force main-thread AppKit and layer resize
    fprintf(stderr, "[native-split] viewport width now %u/%lu\n", width, (unsigned long)guestW);
  }
  if (extraPath) {
    struct RDHostCrop next = {0};
    FILE* ef = fopen(extraPath, "r");
    if (ef) {
      unsigned ex, ey, ew, eh;
      if (fscanf(ef, "%u %u %u %u", &ex, &ey, &ew, &eh) == 4 &&
          ew && eh && ex < guestW && ey < guestH &&
          ew <= guestW - ex && eh <= guestH - ey)
        next = (struct RDHostCrop){ex, ey, ew, eh};
      fclose(ef);
    }
    pthread_mutex_lock(&gHostCropLk);
    int changed = memcmp(&gExtraCrop, &next, sizeof next) != 0;
    if (changed) gExtraCrop = next;
    pthread_mutex_unlock(&gHostCropLk);
    if (changed) {
      atomic_store(&gDrawableW, 0);
      fprintf(stderr, "[native-extra-crop] guest rect %u,%u %ux%u\n",
              next.x, next.y, next.w, next.h);
    }
  }
  if (focusPath) {
    char target[16] = {0};
    FILE* ff = fopen(focusPath, "r");
    if (ff) { (void)fgets(target, sizeof target, ff); fclose(ff); }
    int next = strcmp(target, "linux0\n") == 0 ? 1 :
               strcmp(target, "linux1\n") == 0 ? 2 :
               strcmp(target, "mac\n") == 0 ? 3 : 0;
    if (next && next != atomic_load(&gHostFocusTarget)) {
      atomic_store(&gHostFocusTarget, next);
      atomic_store(&gDrawableW, 0);
      fprintf(stderr, "[native-focus] requested host target %d\n", next);
    }
  }
}
// Refresh rate of the screen the window is on, published by main (0 = unknown).
static atomic_int                 gRefreshHz;

static atomic_int gClosed  = 0;                // set from main (windowWillClose), read anywhere
static atomic_int gFbW     = 0;                // last requested framebuffer size, any thread
static atomic_int gFbH     = 0;

// Content rect in screen coordinates (points, bottom-left origin), published by present().
static pthread_mutex_t gRectLk = PTHREAD_MUTEX_INITIALIZER;
static double gRectX, gRectY, gRectW, gRectH;
static int    gRectValid;

// Research-only native AppKit surface above the nested Linux desktop. This is
// a real WindowServer window: its pixels and input never enter Wayland or SHM.
// AppKit child-window parenting is limited to windows owned by this process.
@interface RDNativeChildView : NSView
@property(nonatomic) BOOL toggled;
@end
@implementation RDNativeChildView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)mouseDown:(NSEvent*)event {
  (void)event; self.toggled = !self.toggled; [self setNeedsDisplay:YES];
  fprintf(stderr, "[native-child] direct AppKit click state=%d\n", self.toggled);
}
- (void)keyDown:(NSEvent*)event {
  if ([event.charactersIgnoringModifiers isEqualToString:@" "]) {
    self.toggled = !self.toggled; [self setNeedsDisplay:YES];
    fprintf(stderr, "[native-child] direct AppKit Space state=%d\n", self.toggled);
  } else [super keyDown:event];
}
- (void)drawRect:(NSRect)rect {
  (void)rect;
  [(self.toggled ? NSColor.systemBlueColor : NSColor.systemRedColor) setFill];
  NSRectFill(self.bounds);
  NSDictionary* attrs = @{NSFontAttributeName: [NSFont systemFontOfSize:20 weight:NSFontWeightMedium],
                          NSForegroundColorAttributeName: NSColor.whiteColor};
  [@"Native AppKit window — click or Space" drawAtPoint:NSMakePoint(20, 20) withAttributes:attrs];
}
@end

static void layout_native_child(void) {
  if (!gWin || !gNativeChild) return;
  NSRect cr = [gWin convertRectToScreen:gWin.contentView.bounds];
  [gNativeChild setFrame:NSMakeRect(NSMidX(cr), NSMinY(cr), NSWidth(cr) * 0.5, NSHeight(cr)) display:YES];
}

static void open_native_child(void) {
  if (!getenv("RDNATIVE_CHILD") || !gWin || gNativeChild) return;
  gNativeChild = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 400)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                  backing:NSBackingStoreBuffered defer:NO];
  gNativeChild.title = @"Real native window over Hyprland";
  gNativeChild.releasedWhenClosed = NO;
  gNativeChild.contentView = [[RDNativeChildView alloc] initWithFrame:gNativeChild.contentView.bounds];
  [gWin addChildWindow:gNativeChild ordered:NSWindowAbove];
  layout_native_child();
  [gNativeChild orderFront:nil];
  fprintf(stderr, "[native-child] AppKit child window %ld parent %ld (no capture)\n",
          (long)gNativeChild.windowNumber, (long)gWin.windowNumber);
  if (getenv("RDNATIVE_CHILD_SELFTEST")) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
      NSEvent* e = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
          location:NSMakePoint(60, 60) modifierFlags:0
          timestamp:NSProcessInfo.processInfo.systemUptime
          windowNumber:gNativeChild.windowNumber context:nil
          eventNumber:0 clickCount:1 pressure:1.0];
      if (e) [NSApp postEvent:e atStart:NO];
      NSEvent* key = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
          modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
          windowNumber:gNativeChild.windowNumber context:nil
          characters:@" " charactersIgnoringModifiers:@" " isARepeat:NO keyCode:49];
      if (key) [NSApp postEvent:key atStart:NO];
    });
  }
}

// ---------------------------------------------------------------- window delegate
static void ev_push(const struct gpuwin_event* e);
static void publish_rect(void);
static void menu_shortcut(uint32_t key) {
  struct gpuwin_event e = {0}; e.type = GPUWIN_EV_KEY;
  e.code = 29; e.state = 1; e.mods = GPUWIN_MOD_CTRL; ev_push(&e);
  e.code = key; e.state = 1; ev_push(&e);
  e.state = 0; ev_push(&e);
  e.code = 29; e.mods = 0; ev_push(&e);
}
@interface RDMenuTarget : NSObject
- (void)copyItem:(id)sender;
- (void)cutItem:(id)sender;
- (void)pasteItem:(id)sender;
- (void)selectAllItems:(id)sender;
@end
@implementation RDMenuTarget
- (void)copyItem:(id)sender { (void)sender; menu_shortcut(46); }
- (void)cutItem:(id)sender { (void)sender; menu_shortcut(45); }
- (void)pasteItem:(id)sender { (void)sender; menu_shortcut(47); }
- (void)selectAllItems:(id)sender { (void)sender; menu_shortcut(30); }
@end
static RDMenuTarget* gMenuTarget;
@interface RDGpuWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation RDGpuWindowDelegate
- (void)windowWillClose:(NSNotification*)n {
  (void)n;
  atomic_store(&gClosed, 1);
  [NSApp terminate:nil];
}
- (void)windowDidResize:(NSNotification*)n {
  (void)n;
  if (gSettingContentSize || !gWin) return;
  NSSize s = gWin.contentView.bounds.size;
  if (s.width < 1 || s.height < 1) return;
  struct gpuwin_event e = {0};
  e.type = GPUWIN_EV_RESIZE;
  e.x = (int32_t)s.width;
  e.y = (int32_t)s.height;
  atomic_store(&gPendingResizeW, e.x);
  atomic_store(&gPendingResizeH, e.y);
  ev_push(&e);
  publish_rect();
  layout_native_child();
}
- (void)windowDidMove:(NSNotification*)n {
  (void)n; publish_rect(); layout_native_child();
}
@end

// ---------------------------------------------------------------- NSApp bring-up
// Non-bundled binaries default to a background activation policy: the window would be created
// but stay behind every other app. Regular + activateIgnoringOtherApps fixes that. Idempotent;
// callable from gpuwin_open() (test harness path) and gpu_run_event_loop() (real path).
// MUST be called on the main thread.
void gpuwin_ensure_app(void) {
  static int done = 0;
  [NSApplication sharedApplication];
  if (done) return;
  done = 1;
  if (getenv("RDMACKEYS")) {
    NSString* name = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleDisplayName"] ?: @"Thunar";
    NSMenu* bar = [NSMenu new];
    NSMenuItem* appItem = [NSMenuItem new]; [bar addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:name]; appItem.submenu = appMenu;
    NSMenuItem* quit = [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name]
                                         action:@selector(terminate:) keyEquivalent:@"q"];
    quit.target = NSApp;
    NSMenuItem* editItem = [NSMenuItem new]; [bar addItem:editItem];
    NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"]; editItem.submenu = edit;
    gMenuTarget = [RDMenuTarget new];
    [edit addItemWithTitle:@"Cut" action:@selector(cutItem:) keyEquivalent:@"x"].target = gMenuTarget;
    [edit addItemWithTitle:@"Copy" action:@selector(copyItem:) keyEquivalent:@"c"].target = gMenuTarget;
    [edit addItemWithTitle:@"Paste" action:@selector(pasteItem:) keyEquivalent:@"v"].target = gMenuTarget;
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAllItems:) keyEquivalent:@"a"].target = gMenuTarget;
    NSApp.mainMenu = bar;
  }
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp activateIgnoringOtherApps:YES];
}

// ---------------------------------------------------------------- pipeline (main thread only)
static NSString* const kBlitMSL =
  @"#include <metal_stdlib>\n"
   "using namespace metal;\n"
   "struct VOut { float4 pos [[position]]; float2 uv; };\n"
   "// Fullscreen triangle: no vertex buffer, UVs straight out of vertex_id.\n"
   "//   vid 0 -> uv(0,0)  vid 1 -> uv(2,0)  vid 2 -> uv(0,2)\n"
   "// uv(0,0) maps to clip (-1,+1) = top-left, so texture row 0 is the top row (X11 order).\n"
   "vertex VOut v_blit(uint vid [[vertex_id]]) {\n"
   "  float2 uv = float2(float((vid << 1) & 2u), float(vid & 2u));\n"
   "  VOut o;\n"
   "  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
   "  o.uv  = uv;\n"
   "  return o;\n"
   "}\n"
   "fragment float4 f_blit(VOut in [[stage_in]],\n"
   "                       texture2d<float> tex [[texture(0)]],\n"
   "                       sampler smp [[sampler(0)]],\n"
   "                       constant float4& uvRect [[buffer(0)]]) {\n"
   "  // Force opaque: a GL frame straight off glbridge carries the app's alpha, and glxgears\n"
   "  // never sets a clear colour, so its background alpha is 0.\n"
   "  return float4(tex.sample(smp, uvRect.xy + in.uv * uvRect.zw).rgb, 1.0);\n"
   "}\n";

static int ensure_pipeline(id<MTLDevice> dev) {
  if (gPipe) return 0;
  NSError* err = nil;
  id<MTLLibrary> lib = [dev newLibraryWithSource:kBlitMSL options:nil error:&err];
  if (!lib) {
    fprintf(stderr, "[gpuwin] shader compile failed: %s\n",
            err ? err.description.UTF8String : "?");
    return -5;
  }
  MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
  d.vertexFunction   = [lib newFunctionWithName:@"v_blit"];
  d.fragmentFunction = [lib newFunctionWithName:@"f_blit"];
  d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  gPipe = [dev newRenderPipelineStateWithDescriptor:d error:&err];
  if (!gPipe) {
    fprintf(stderr, "[gpuwin] pipeline failed: %s\n", err ? err.description.UTF8String : "?");
    return -5;
  }
  MTLSamplerDescriptor* s = [MTLSamplerDescriptor new];
  s.minFilter = MTLSamplerMinMagFilterLinear;
  s.magFilter = MTLSamplerMinMagFilterLinear;
  s.sAddressMode = MTLSamplerAddressModeClampToEdge;
  s.tAddressMode = MTLSamplerAddressModeClampToEdge;
  gSampler = [dev newSamplerStateWithDescriptor:s];
  return gSampler ? 0 : -5;
}

// ---------------------------------------------------------------- window (main thread only)
static long open_on_main(NSUInteger w, NSUInteger h);
static int  inflight_limit(void);
static int  novsync(void);

static void size_dynamic_regions(NSUInteger guestW, NSUInteger guestH) {
  if (!dynamic_regions_enabled()) return;
  struct RDRegionSpec* desired = NULL;
  size_t count = 0;
  pthread_mutex_lock(&gRegionLock);
  count = gDesiredRegionCount;
  if (count) {
    desired = malloc(count * sizeof *desired);
    if (!desired) { pthread_mutex_unlock(&gRegionLock); return; }
    memcpy(desired, gDesiredRegions, count * sizeof *desired);
  }
  pthread_mutex_unlock(&gRegionLock);
  if (!gRegionsByID) gRegionsByID = [NSMutableDictionary new];
  NSMutableSet<NSNumber*>* wanted = [NSMutableSet new];
  NSMutableArray<RDRegionFrame*>* frames = [NSMutableArray new];
  for (size_t i = 0; i < count; i++) {
    struct RDRegionSpec spec = desired[i];
    NSNumber* key = @(spec.id);
    [wanted addObject:key];
    RDRegionFrame* old = gRegionsByID[key];
    NSWindow* win = old.window;
    CAMetalLayer* layer = old.layer;
    if (!win) {
      win = [[RDPrimaryGuestWindow alloc]
          initWithContentRect:NSMakeRect(0, 0, spec.crop.w, spec.crop.h)
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered defer:NO];
      if (!win) continue;
      win.title = [NSString stringWithFormat:@"Linux 0x%llx", (unsigned long long)spec.id];
      win.releasedWhenClosed = NO;
      win.acceptsMouseMovedEvents = YES;
      NSView* view = win.contentView;
      view.wantsLayer = YES;
      layer = [CAMetalLayer layer];
      layer.device = gpu_metal_device();
      layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
      layer.framebufferOnly = YES;
      layer.opaque = YES;
      layer.maximumDrawableCount = (inflight_limit() >= 3) ? 3 : 2;
      layer.displaySyncEnabled = novsync() ? NO : YES;
      view.layer = layer;
      fprintf(stderr, "[native-region] created id=0x%llx window=%ld\n",
              (unsigned long long)spec.id, (long)win.windowNumber);
    }
    if ((NSUInteger)win.contentView.bounds.size.width != spec.crop.w ||
        (NSUInteger)win.contentView.bounds.size.height != spec.crop.h)
      [win setContentSize:NSMakeSize(spec.crop.w, spec.crop.h)];
    // Host-generated Wlr layer IDs have bit 62. The replacement shell must
    // sit above the Apple-bar cover, while ordinary app popups remain floating.
    NSWindowLevel level = (spec.panel > 0 && (spec.id & (1ULL << 62)) &&
                           !(spec.id & (1ULL << 63))) ?
                          CGWindowLevelForKey(kCGStatusWindowLevelKey) + 2 :
                          spec.panel > 0 ? NSFloatingWindowLevel :
                          spec.panel < 0 ? NSNormalWindowLevel - 1 : NSNormalWindowLevel;
    if (win.level != level) [win setLevel:level];
    NSPoint pos = NSMakePoint(gFullOriginX + spec.crop.x,
                             gFullOriginY + (CGFloat)guestH - spec.crop.y - spec.crop.h);
    if (!NSEqualPoints(win.frame.origin, pos)) [win setFrameOrigin:pos];
    layer.frame = win.contentView.bounds;
    layer.drawableSize = CGSizeMake(spec.crop.w, spec.crop.h);
    [win orderFront:nil];
    RDRegionFrame* frame = [RDRegionFrame new];
    frame.window = win; frame.layer = layer;
    frame.ident = spec.id; frame.crop = spec.crop; frame.panel = spec.panel > 0;
    frame.screenFrame = win.frame;
    frame.scale = win.backingScaleFactor > 0.5 ? win.backingScaleFactor : 1.0;
    gRegionsByID[key] = frame;
    [frames addObject:frame];
  }
  free(desired);
  for (NSNumber* key in [gRegionsByID allKeys]) {
    if ([wanted containsObject:key]) continue;
    RDRegionFrame* gone = gRegionsByID[key];
    [gone.window orderOut:nil];
    [gRegionsByID removeObjectForKey:key];
    fprintf(stderr, "[native-region] removed id=0x%llx\n", key.unsignedLongLongValue);
  }
  [gWin orderOut:nil];
  pthread_mutex_lock(&gRegionLock);
  gRegionFrames = [frames copy];
  pthread_mutex_unlock(&gRegionLock);
  atomic_store(&gRegionsDirty, 0);
  gRegionOutputW = guestW; gRegionOutputH = guestH;
  gAppliedDynamicFocusID = 0;
  gAppliedDynamicFocusMac = 0;
}

static void apply_dynamic_focus(void) {
  if (!dynamic_regions_enabled()) return;
  int mac = atomic_load(&gDynamicFocusMac);
  uint64_t id = atomic_load(&gDynamicFocusID);
  if (mac == gAppliedDynamicFocusMac && id == gAppliedDynamicFocusID) return;
  if (mac) {
    // Move top to bottom so the Linux windows keep their relative stacking
    // while the selected native Mac window comes forward.
    for (RDRegionFrame* region in [gRegionFrames reverseObjectEnumerator])
      if (!region.panel) [region.window orderBack:nil];
  } else if (id) {
    RDRegionFrame* selected = gRegionsByID[@(id)];
    if (!selected) return;
    [NSApp activateIgnoringOtherApps:YES];
    [selected.window makeKeyAndOrderFront:nil];
  }
  gAppliedDynamicFocusMac = mac;
  gAppliedDynamicFocusID = id;
  fprintf(stderr, "[native-region] focus %s0x%llx\n", mac ? "mac " : "linux ",
          (unsigned long long)id);
}

static NSScreen* screen_for_display_id(const char* requested) {
  if (!requested || !*requested) return nil;
  char* end = NULL;
  unsigned long wanted = strtoul(requested, &end, 10);
  if (!end || *end || wanted == 0 || wanted > UINT32_MAX) return nil;
  for (NSScreen* screen in NSScreen.screens) {
    NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
    if (number && number.unsignedLongValue == wanted) return screen;
  }
  return nil;
}

// The screen the full-screen primary output should fill, mirroring the launcher's
// `auto` choice: the explicitly requested display if it is still connected, else the
// first external (non-builtin) panel, else the builtin, else the main screen. Never
// returns nil while any display is attached, so an unplugged primary re-fits to the
// remaining display instead of leaving the window on a now-dead screen.
static NSScreen* hotplug_target_screen(void) {
  const char* requested = getenv("RDNATIVE_TARGET_DISPLAY_ID");
  NSScreen* screen = screen_for_display_id(requested);
  if (screen) return screen;
  if (requested && *requested)
    fprintf(stderr, "[hotplug] requested display %s gone; choosing a remaining display\n", requested);
  NSScreen* builtin = nil;
  for (NSScreen* s in NSScreen.screens) {
    NSNumber* n = s.deviceDescription[@"NSScreenNumber"];
    CGDirectDisplayID did = n ? (CGDirectDisplayID)n.unsignedIntValue : 0;
    if (did && CGDisplayIsBuiltin(did)) builtin = s;
    else return s;                                  // first external panel
  }
  return builtin ?: NSScreen.mainScreen;
}

// ---------------------------------------------------------------- display hotplug
// A CGDisplay reconfiguration fires when a display is plugged, unplugged, mirrored,
// or reordered. The callback can run on any thread and often fires several times per
// physical event, so it is marshalled to main and debounced: only the last change in
// a 200 ms window triggers a re-fit.
static _Atomic(uint64_t) gHotplugGeneration;

static void hotplug_refit(void) {
  if (!split_output_enabled() || !fit_display_enabled()) {
    fprintf(stderr, "[hotplug] display change ignored (not a full-screen fit layout)\n");
    return;
  }
  NSScreen* target = hotplug_target_screen();
  if (!target) {
    fprintf(stderr, "[hotplug] no active display remains; hiding full-screen window\n");
    if (gWin && gWin.isVisible) [gWin orderOut:nil];
    atomic_store(&gDrawableW, 0);
    atomic_store(&gDrawableH, 0);
    return;
  }
  NSNumber* num = target.deviceDescription[@"NSScreenNumber"];
  fprintf(stderr, "[hotplug] re-fit full-screen output to display %u %.0fx%.0f@%.1f\n",
          (unsigned)num.unsignedIntValue, target.frame.size.width, target.frame.size.height,
          (double)target.backingScaleFactor);
  // Invalidate the drawable size so the next presented frame re-runs
  // ensure_ready_on_main() -> size_layer(), which re-reads NSScreen.screens on the
  // main thread: the primary window moves to the still-connected display and
  // size_extra_crop() drops or re-adds the second-display mirror. We deliberately do
  // NOT call size_layer() directly here — that would race the presenter thread's
  // -[CAMetalLayer nextDrawable]. The desktop's bar clock + clients damage frames
  // continuously, so the re-fit applies within a frame or two.
  atomic_store(&gDrawableW, 0);
  atomic_store(&gDrawableH, 0);
}

static void display_reconfig_callback(CGDirectDisplayID display,
                                      CGDisplayChangeSummaryFlags flags,
                                      void* userInfo) {
  (void)display; (void)userInfo;
  const CGDisplayChangeSummaryFlags relevant =
      kCGDisplayAddFlag | kCGDisplayRemoveFlag |
      kCGDisplayEnabledFlag | kCGDisplayDisabledFlag |
      kCGDisplaySetMainFlag | kCGDisplayMovedFlag |
      kCGDisplayMirrorFlag | kCGDisplayUnMirrorFlag |
      kCGDisplayDesktopShapeChangedFlag;
  if (!(flags & relevant)) return;
  uint64_t gen = atomic_fetch_add(&gHotplugGeneration, 1) + 1;
  // Give NSScreen.screens a beat to catch up with the CG configuration change.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(200 * NSEC_PER_MSEC)),
                 dispatch_get_main_queue(), ^{
    if (atomic_load(&gHotplugGeneration) != gen) return;   // superseded by a newer event
    hotplug_refit();
  });
}

static void start_hotplug_watch(void) {
  static int registered;
  if (registered) return;
  registered = 1;
  CGDisplayRegisterReconfigurationCallback(display_reconfig_callback, NULL);
  fprintf(stderr, "[hotplug] display reconfiguration callback registered\n");
}

// RDNATIVE_HOTPLUG_TEST=1 fires one synthetic re-fit after 2 s to prove the hotplug
// path logs and re-fits without a physical display change.
static void start_hotplug_selftest(void) {
  if (!getenv("RDNATIVE_HOTPLUG_TEST")) return;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
    fprintf(stderr, "[hotplug] simulated display change (RDNATIVE_HOTPLUG_TEST)\n");
    hotplug_refit();
  });
}

static void size_extra_crop(NSUInteger w, NSUInteger h) {
  const int hasCropFile = getenv("RDNATIVE_EXTRA_CROP_PATH") != NULL;
  const int hasExtraDisplay = getenv("RDNATIVE_EXTRA_TARGET_DISPLAY_ID") != NULL;
  if ((!hasCropFile && !hasExtraDisplay) || !split_output_enabled()) return;

  // Full-screen second-display mode: fill a DIFFERENT display with the WHOLE
  // source, scaled to that display. This is what turns the one compositor
  // IOSurface into "the same desktop on both monitors". A missing requested
  // display is an explicit hide, never a fallback onto the primary display.
  NSScreen* extraScreen = hasExtraDisplay ?
      screen_for_display_id(getenv("RDNATIVE_EXTRA_TARGET_DISPLAY_ID")) : nil;
  if (hasExtraDisplay && !extraScreen) {
    fprintf(stderr, "[native-extra] requested display %s unavailable; hidden\n",
            getenv("RDNATIVE_EXTRA_TARGET_DISPLAY_ID"));
    atomic_store(&gExtraCropVisible, 0);
    if (gExtraCropWin.isVisible) {
      [gExtraCropWin orderOut:nil];
      gAppliedHostFocus = 0;
    }
    pthread_mutex_lock(&gRectLk);
    gExtraRectW = gExtraRectH = 0;
    pthread_mutex_unlock(&gRectLk);
    return;
  }
  struct RDHostCrop crop;
  if (hasExtraDisplay) {
    crop = (struct RDHostCrop){0, 0, (unsigned)w, (unsigned)h};
  } else {
    crop = extra_crop_snapshot(w, h);
    if (!crop.w) {
      atomic_store(&gExtraCropVisible, 0);
      if (gExtraCropWin.isVisible) {
        [gExtraCropWin orderOut:nil];
        gAppliedHostFocus = 0;
        fprintf(stderr, "[native-extra-crop] host window hidden\n");
      }
      pthread_mutex_lock(&gRectLk);
      gExtraRectW = gExtraRectH = 0;
      pthread_mutex_unlock(&gRectLk);
      return;
    }
  }

  NSSize  winSize = hasExtraDisplay ? extraScreen.frame.size : NSMakeSize(crop.w, crop.h);
  NSPoint pos     = hasExtraDisplay ? extraScreen.frame.origin :
                    NSMakePoint(gFullOriginX + crop.x,
                                gFullOriginY + (CGFloat)h - crop.y - crop.h);
  CGFloat scale   = hasExtraDisplay ? extraScreen.backingScaleFactor : 1.0;

  if (!gExtraCropWin) {
    gExtraCropWin = [[RDPrimaryGuestWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, winSize.width, winSize.height)
                styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered defer:NO];
    if (!gExtraCropWin) return;
    gExtraCropWin.title = @"Linux region";
    gExtraCropWin.releasedWhenClosed = NO;
    gExtraCropWin.acceptsMouseMovedEvents = YES;
    if (kiosk_enabled()) gExtraCropWin.level = kiosk_window_level();
    NSView* view = gExtraCropWin.contentView;
    view.wantsLayer = YES;
    gExtraCropLayer = [CAMetalLayer layer];
    gExtraCropLayer.device = gpu_metal_device();
    gExtraCropLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    gExtraCropLayer.framebufferOnly = YES;
    gExtraCropLayer.opaque = YES;
    gExtraCropLayer.maximumDrawableCount = (inflight_limit() >= 3) ? 3 : 2;
    gExtraCropLayer.displaySyncEnabled = novsync() ? NO : YES;
    view.layer = gExtraCropLayer;
  }
  if (!gExtraCropLayer) return;
  if ((NSUInteger)gExtraCropWin.contentView.bounds.size.width != (NSUInteger)winSize.width ||
      (NSUInteger)gExtraCropWin.contentView.bounds.size.height != (NSUInteger)winSize.height)
    [gExtraCropWin setContentSize:winSize];
  if (!NSEqualPoints(gExtraCropWin.frame.origin, pos)) [gExtraCropWin setFrameOrigin:pos];
  gExtraCropLayer.frame = gExtraCropWin.contentView.bounds;
  if (hasExtraDisplay) gExtraCropLayer.contentsScale = scale;
  gExtraCropLayer.drawableSize = CGSizeMake((CGFloat)winSize.width * scale,
                                            (CGFloat)winSize.height * scale);
  if (!gExtraCropWin.isVisible) {
    [gExtraCropWin orderFront:nil];
    gAppliedHostFocus = 0;
  }
  pthread_mutex_lock(&gRectLk);
  gExtraRectX = pos.x; gExtraRectY = pos.y;
  gExtraRectW = winSize.width; gExtraRectH = winSize.height;
  pthread_mutex_unlock(&gRectLk);
  if (hasExtraDisplay) {
    pthread_mutex_lock(&gHostCropLk);
    int changed = memcmp(&gExtraCrop, &crop, sizeof crop) != 0;
    if (changed) gExtraCrop = crop;
    pthread_mutex_unlock(&gHostCropLk);
    static int announced;
    if (!announced++ || changed) {
      NSNumber* displayNumber = extraScreen.deviceDescription[@"NSScreenNumber"];
      fprintf(stderr, "[native-extra] full-screen second display %lu %.0fx%.0f@%.1f "
              "drawable %.0fx%.0f source %ux%u\n",
              (unsigned long)displayNumber.unsignedIntValue, winSize.width, winSize.height,
              (double)scale, (double)winSize.width * scale, (double)winSize.height * scale,
              (unsigned)w, (unsigned)h);
    }
  }
  atomic_store(&gExtraCropVisible, 1);
}

static void apply_host_focus(void) {
  int target = atomic_load(&gHostFocusTarget);
  if (!target || target == gAppliedHostFocus) return;
  if (target == 3) {
    [gWin orderBack:nil];
    [gExtraCropWin orderBack:nil];
  } else {
    [NSApp activateIgnoringOtherApps:YES];
    NSWindow* selected = target == 2 && gExtraCropWin.isVisible ? gExtraCropWin : gWin;
    [selected makeKeyAndOrderFront:nil];
  }
  gAppliedHostFocus = target;
  fprintf(stderr, "[native-focus] applied host target %d\n", target);
}

static void size_layer(NSUInteger w, NSUInteger h) {
  NSView* v = gWin.contentView;
  if (split_output_enabled() && fit_display_enabled()) {
    NSScreen* target = hotplug_target_screen();
    if (target) {
      gFullOriginX = NSMinX(target.frame);
      gFullOriginY = NSMinY(target.frame);
      gFitDisplayW = (NSUInteger)target.frame.size.width;
      gFitDisplayH = (NSUInteger)target.frame.size.height;
      gFitDisplayScale = target.backingScaleFactor;
    }
  }
  if (gCAPresLayer) {
    // The CA path's layer takes the guest's pixels as CONTENTS, so it has no drawableSize; what
    // it needs instead is a contentsScale that maps w x h PIXELS onto the view's POINT bounds.
    // Leaving it at the screen's scale on a Retina panel displays the top-left quarter of the
    // frame at 2x and looks like a viewport bug rather than a scale bug.
    gCAPresLayer.frame = v.bounds;
    CGFloat bw = v.bounds.size.width;
    gCAPresLayer.contentsScale = (bw > 0) ? (CGFloat)w / bw : 1.0;
    atomic_store(&gDrawableW, (unsigned long)w);
    atomic_store(&gDrawableH, (unsigned long)h);
    return;
  }
  if (split_output_enabled()) {
    if (fit_display_enabled()) {
      // Full-screen fit: the borderless primary window is exactly the fitted
      // display's frame in points, and the whole guest framebuffer is scaled into
      // it (no crop). size_extra_crop() below still manages the second-display
      // mirror independently, so unplug/replug of the mirror is handled too.
      NSUInteger fw = gFitDisplayW ?: host_output_width(w);
      NSUInteger fh = gFitDisplayH ?: host_output_height(h);
      if ((NSUInteger)gWin.contentView.bounds.size.width != fw ||
          (NSUInteger)gWin.contentView.bounds.size.height != fh) {
        gSettingContentSize = 1;
        [gWin setContentSize:NSMakeSize((CGFloat)fw, (CGFloat)fh)];
        gSettingContentSize = 0;
      }
      if (!NSEqualPoints(gWin.frame.origin, NSMakePoint(gFullOriginX, gFullOriginY)))
        [gWin setFrameOrigin:NSMakePoint(gFullOriginX, gFullOriginY)];
      publish_rect();
    } else {
      struct RDHostCrop crop = host_crop_snapshot(w, h);
      if ((NSUInteger)gWin.contentView.bounds.size.width != crop.w ||
          (NSUInteger)gWin.contentView.bounds.size.height != crop.h) {
        gSettingContentSize = 1;
        [gWin setContentSize:NSMakeSize((CGFloat)crop.w, (CGFloat)crop.h)];
        gSettingContentSize = 0;
      }
      NSPoint target = NSMakePoint(gFullOriginX + crop.x,
                                  gFullOriginY + (CGFloat)h - crop.y - crop.h);
      if (!NSEqualPoints(gWin.frame.origin, target)) [gWin setFrameOrigin:target];
      publish_rect();
    }
  }
  gLayer.frame        = v.bounds;
  gLayer.drawableSize = expected_drawable_size(w, h);
  if (dynamic_regions_enabled()) {
    if (atomic_load(&gRegionsDirty) || gRegionOutputW != w || gRegionOutputH != h)
      size_dynamic_regions(w, h);
    apply_dynamic_focus();
  }
  else { size_extra_crop(w, h); apply_host_focus(); }
  atomic_store(&gDrawableW, (unsigned long)w);
  atomic_store(&gDrawableH, (unsigned long)h);
}

// RDGLNOVSYNC=1 asks CoreAnimation not to hold the layer to the display refresh. Off by
// default — vsync is what makes the animation evenly paced. Note that on macOS 26 this did not
// actually lift the cap for a layer-backed NSView here; it is kept as the documented knob.
static int novsync(void) {
  static int v = -1;
  if (v < 0) { const char* e = getenv("RDGLNOVSYNC"); v = (e && *e && *e != '0'); }
  return v;
}

// RDGLSTATS=1 also reports how much of a GL present is spent waiting for a drawable — i.e. how
// much of the frame is display pacing rather than work we could remove.
static int glstats_on(void) {
  static int v = -1;
  if (v < 0) { const char* e = getenv("RDGLSTATS"); v = (e && *e && *e != '0'); }
  return v;
}

static double gWaitAcc;                            // nextDrawable wait, ms, since last report

static void note_drawable_wait(uint64_t elapsed_ticks) {
  static mach_timebase_info_data_t tb;
  if (tb.denom == 0) mach_timebase_info(&tb);
  gWaitAcc += (double)elapsed_ticks * tb.numer / tb.denom / 1.0e6;
}

// ---- what the DISPLAY actually did -----------------------------------------------------
// The interval between successive glb_swap/gpuwin_present calls is a producer-side number: with
// several frames in flight the producer can bunch two frames together and still have them shown
// one refresh apart. -[MTLDrawable presentedTime] is the ground truth — when the frame really
// went up — so jank is measured here and nowhere else.
#define PRESENTED_CAP 2048
static pthread_mutex_t gPresLk = PTHREAD_MUTEX_INITIALIZER;
static double gPresPrev;                           // previous presentedTime (seconds)
static float  gPresInt[PRESENTED_CAP];             // intervals, ms
static int    gPresN;
static int    gPresMissed;                         // presentedTime == 0: never made it up

static int pres_cmp(const void* a, const void* b) {
  float x = *(const float*)a, y = *(const float*)b;
  return (x < y) ? -1 : (x > y);
}

static void presented_report(void) {
  int n = gPresN;
  if (n < 8) return;
  static float tmp[PRESENTED_CAP];
  memcpy(tmp, gPresInt, sizeof(float) * (size_t)n);
  qsort(tmp, (size_t)n, sizeof(float), pres_cmp);
  float p50 = tmp[n / 2], p95 = tmp[(int)(n * 0.95)], p99 = tmp[(int)(n * 0.99)];
  double sum = 0;
  for (int i = 0; i < n; i++) sum += tmp[i];

  // Histogram in half-refresh steps: 1.0x is "shown on the very next refresh".
  int bucket[9]; memset(bucket, 0, sizeof bucket);
  for (int i = 0; i < n; i++) {
    int b = (int)(gPresInt[i] / p50 * 2.0 + 0.5) - 1;
    if (b < 0) b = 0;
    if (b > 8) b = 8;
    bucket[b]++;
  }
  fprintf(stderr,
          "[gpuwin] PRESENTED %d frames: %.1f/s | ms p50 %.2f p95 %.2f p99 %.2f max %.2f | "
          "nextDrawable %.2f ms | missed %d\n",
          n, sum > 0 ? n / (sum / 1000.0) : 0.0, p50, p95, p99, tmp[n - 1],
          gWaitAcc / n, gPresMissed);
  fprintf(stderr, "[gpuwin] presented refresh multiples:");
  for (int b = 0; b < 9; b++)
    if (bucket[b]) fprintf(stderr, "  %.1fx:%.1f%%", (b + 1) * 0.5, 100.0 * bucket[b] / n);
  fputc('\n', stderr);

  gPresN = 0; gPresMissed = 0; gWaitAcc = 0;
}

// Called from the drawable's presented handler (an internal Metal queue).
static void note_presented(double t) {
  pthread_mutex_lock(&gPresLk);
  if (t <= 0) {
    gPresMissed++;                                 // dropped: never reached the display
  } else {
    if (gPresPrev > 0 && gPresN < PRESENTED_CAP)
      gPresInt[gPresN++] = (float)((t - gPresPrev) * 1000.0);
    gPresPrev = t;
    if (gPresN >= 600) presented_report();
  }
  pthread_mutex_unlock(&gPresLk);
}

// Put the window on the BUILT-IN panel, centred. -[NSWindow center] follows the "main" screen,
// which is whichever screen owns the key window — so with an external display attached the
// window lands there, on a different refresh domain and an extra compositing hop. Main thread.
static void center_on_builtin(NSWindow* win) {
  NSScreen* target = nil;
  const char* selected = getenv("RDNATIVE_TARGET_DISPLAY_ID");
  if (selected && *selected) {
    char* end = NULL;
    unsigned long requested = strtoul(selected, &end, 10);
    if (end && !*end && requested > 0 && requested <= UINT32_MAX) {
      for (NSScreen* s in [NSScreen screens]) {
        NSNumber* n = s.deviceDescription[@"NSScreenNumber"];
        if (n && n.unsignedIntValue == requested) { target = s; break; }
      }
    }
  }
  if (selected && *selected && !target)
    fprintf(stderr, "[native-desktop] requested display %s unavailable; using built-in\n", selected);
  if (!target) for (NSScreen* s in [NSScreen screens]) {
      NSNumber* n = s.deviceDescription[@"NSScreenNumber"];
      if (n && CGDisplayIsBuiltin((CGDirectDisplayID)n.unsignedIntValue)) { target = s; break; }
    }
  if (!target) { [win center]; return; }            // headless / no internal panel
  NSRect vis = target.visibleFrame, fr = win.frame;
  [win setFrameOrigin:NSMakePoint(vis.origin.x + (vis.size.width  - fr.size.width)  * 0.5,
                                  vis.origin.y + (vis.size.height - fr.size.height) * 0.5)];
}

// Publish the content rect in screen coords for gpuwin_cursor(). Main thread only.
static void publish_rect(void) {
  if (!gWin) return;
  NSRect scr = [gWin convertRectToScreen:gWin.contentView.frame];
  int changed;
  pthread_mutex_lock(&gRectLk);
  changed = !gRectValid || gRectX != scr.origin.x || gRectY != scr.origin.y ||
            gRectW != scr.size.width || gRectH != scr.size.height;
  gRectX = scr.origin.x; gRectY = scr.origin.y;
  gRectW = scr.size.width; gRectH = scr.size.height;
  gRectValid = 1;
  pthread_mutex_unlock(&gRectLk);

  // One-way, opt-in geometry publication for a separate native app window.
  // No pixels or input events cross this boundary.
  const char* geometryPath = getenv("RDNATIVE_GEOMETRY_PATH");
  if (changed && geometryPath && geometryPath[0] == '/') {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", geometryPath) < (int)sizeof tmp) {
      FILE* f = fopen(tmp, "w");
      if (f) {
        fprintf(f, "%.3f %.3f %.3f %.3f\n", scr.origin.x, scr.origin.y,
                scr.size.width, scr.size.height);
        fclose(f);
        rename(tmp, geometryPath);
      }
    }
  }
  const char* fullPath = getenv("RDNATIVE_FULL_GEOMETRY_PATH");
  if (dynamic_regions_enabled() && fullPath && fullPath[0] == '/') {
    static double lastX, lastY;
    static int lastW, lastH, haveFull;
    int width = atomic_load(&gFbW), height = atomic_load(&gFbH);
    if (width > 0 && height > 0 &&
        (!haveFull || lastX != gFullOriginX || lastY != gFullOriginY ||
         lastW != width || lastH != height)) {
      char tmp[PATH_MAX];
      if (snprintf(tmp, sizeof tmp, "%s.tmp", fullPath) < (int)sizeof tmp) {
        FILE* f = fopen(tmp, "w");
        if (f) {
          fprintf(f, "%.3f %.3f %d %d\n", gFullOriginX, gFullOriginY, width, height);
          fclose(f);
          if (rename(tmp, fullPath) == 0) {
            lastX = gFullOriginX; lastY = gFullOriginY;
            lastW = width; lastH = height; haveFull = 1;
          }
        }
      }
    }
  }

  // The window can be dragged between displays with different refresh rates, so this is
  // re-read here rather than latched once.
  NSScreen* s = gWin.screen;
  if (s) atomic_store(&gRefreshHz, (int)s.maximumFramesPerSecond);
}

// Everything that must be true before a frame can be presented, and all of it Cocoa: the
// window, the blit pipeline and the drawable size. Main thread only; cheap when already right.
static long ensure_ready_on_main(NSUInteger w, NSUInteger h) {
  if (!gWin || (!gLayer && !gCAPresLayer)) {
    long rc = open_on_main(w, h);
    if (rc < 0) return rc;
  }
  // On the CA path there is no Metal pipeline, no drawable and no drawable pool to size: the
  // guest's pixels reach the screen as layer contents. Everything below this line is
  // CAMetalLayer-specific and would crash or lie on a plain CALayer.
  if (gCAPresLayer) {
    if (atomic_load(&gDrawableW) != (unsigned long)w || atomic_load(&gDrawableH) != (unsigned long)h)
      size_layer(w, h);
    publish_rect();
    return 0;
  }
  id<MTLDevice> dev = gpu_metal_device();
  if (!dev) return -19;
  long rc = ensure_pipeline(dev);
  if (rc < 0) return rc;
  CGSize expected = expected_drawable_size(w, h);
  if (atomic_load(&gDrawableW) != (unsigned long)w ||
      atomic_load(&gDrawableH) != (unsigned long)h ||
      (NSUInteger)gLayer.drawableSize.width != (NSUInteger)expected.width ||
      (NSUInteger)gLayer.drawableSize.height != (NSUInteger)expected.height)
    size_layer(w, h);
  // The drawable pool has to be at least as deep as the frames we allow in flight (CAMetalLayer
  // only accepts 2 or 3).
  gLayer.maximumDrawableCount = (inflight_limit() >= 3) ? 3 : 2;
  gLayer.displaySyncEnabled   = novsync() ? NO : YES;
  publish_rect();
  return 0;
}

static long open_on_main(NSUInteger w, NSUInteger h) {
  gpuwin_ensure_app();
  kiosk_suppress_screensaver();
  start_dynamic_region_watchers();
  start_hotplug_watch();
  start_hotplug_selftest();

  id<MTLDevice> dev = gpu_metal_device();
  if (!dev) { fprintf(stderr, "[gpuwin] no Metal device\n"); return -19; /* -ENODEV */ }

  if (gWin) {                                     // idempotent: resize, don't spawn a twin
    NSRect fr = [gWin frameRectForContentRect:NSMakeRect(0, 0, (CGFloat)w, (CGFloat)h)];
    int pending_w = atomic_load(&gPendingResizeW), pending_h = atomic_load(&gPendingResizeH);
    if (!pending_w || ((int)w == pending_w && (int)h == pending_h)) {
      gSettingContentSize = 1;
      [gWin setContentSize:NSMakeSize((CGFloat)host_output_width(w), (CGFloat)host_output_height(h))];
      gSettingContentSize = 0;
      atomic_store(&gPendingResizeW, 0); atomic_store(&gPendingResizeH, 0);
    }
    (void)fr;
    size_layer(w, h);
    if (dynamic_regions_enabled()) [gWin orderOut:nil];
    else [gWin makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    return 0;
  }

  NSRect content = NSMakeRect(0, 0, (CGFloat)host_output_width(w), (CGFloat)host_output_height(h));
  NSWindowStyleMask mask = (getenv("RDWL_CLIENT_DECORATIONS") || split_output_enabled()) ?
      NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable :
      NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
  Class windowClass = (getenv("RDWL_CLIENT_DECORATIONS") || split_output_enabled()) ? RDPrimaryGuestWindow.class : NSWindow.class;
  gWin = [[windowClass alloc] initWithContentRect:content
                                     styleMask:mask
                                       backing:NSBackingStoreBuffered
                                         defer:NO];
  if (!gWin) return -12;                          // -ENOMEM
  if (getenv("RDWL_CLIENT_DECORATIONS"))
    fprintf(stderr, "[wl-host] primary window uses guest decorations (borderless AppKit frame)\n");
  gWin.title = gWantedTitle ?: @"Linux Application";
  gWin.releasedWhenClosed = NO;                   // we keep the strong ref
  center_on_builtin(gWin);
  if (kiosk_enabled()) {
    gWin.level = kiosk_window_level();
    fprintf(stderr, "[kiosk] desktop window raised to level %ld (above menu bar + Dock)\n",
            (long)gWin.level);
  }
  // Kiosk UI suppression: each cover window is created once per NSScreen so BOTH
  // displays are masked. The single-window version left the second display's menu
  // bar / Dock / wallpaper exposed. All covers are process-scoped and vanish on exit.
  NSColor* kioskCover = [NSColor colorWithCalibratedRed:0.045 green:0.055 blue:0.075 alpha:1];
  if (getenv("RDNATIVE_DESKTOP_BACKGROUND")) {
    for (NSScreen* screen in NSScreen.screens) {
      NSWindow* cover = [[NSWindow alloc] initWithContentRect:screen.frame
          styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
      cover.releasedWhenClosed = NO;
      cover.backgroundColor = kioskCover;
      cover.opaque = YES;
      cover.ignoresMouseEvents = YES;
      cover.level = CGWindowLevelForKey(kCGDesktopIconWindowLevelKey) + 1;
      [cover orderFront:nil];
      if (!gDesktopBackgrounds) gDesktopBackgrounds = [NSMutableArray new];
      [gDesktopBackgrounds addObject:cover];
      NSNumber* display = screen.deviceDescription[@"NSScreenNumber"];
      fprintf(stderr, "[native-desktop] reversible background window %ld level %ld display %u\n",
              (long)cover.windowNumber, (long)cover.level, display.unsignedIntValue);
    }
  }
  if (getenv("RDNATIVE_MENU_MASK")) {
    for (NSScreen* screen in NSScreen.screens) {
      // Bounded session experiment: cover the system menu/status strip across
      // native-app activation. This does not disable WindowServer or relocate
      // its status items. The windows vanish when this shim process exits.
      CGFloat inset = NSMaxY(screen.frame) - NSMaxY(screen.visibleFrame);
      CGFloat height = inset >= 20 && inset <= 64 ? inset : 32;
      NSRect strip = NSMakeRect(NSMinX(screen.frame), NSMaxY(screen.frame) - height,
                                NSWidth(screen.frame), height);
      NSWindow* mask = [[NSWindow alloc] initWithContentRect:strip
          styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
      mask.releasedWhenClosed = NO;
      mask.backgroundColor = kioskCover;
      mask.opaque = YES;
      mask.hasShadow = NO;
      mask.hidesOnDeactivate = NO;
      mask.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                NSWindowCollectionBehaviorStationary;
      mask.level = CGWindowLevelForKey(kCGStatusWindowLevelKey) + 1;
      [mask orderFront:nil];
      if (!gMenuMasks) gMenuMasks = [NSMutableArray new];
      [gMenuMasks addObject:mask];
      NSNumber* display = screen.deviceDescription[@"NSScreenNumber"];
      fprintf(stderr, "[native-desktop] temporary menu mask window %ld level %ld display %u frame %.0f %.0f %.0f %.0f\n",
              (long)mask.windowNumber, (long)mask.level, display.unsignedIntValue,
              strip.origin.x, strip.origin.y, strip.size.width, strip.size.height);
    }
  }
  if (getenv("RDNATIVE_DOCK_MASK")) {
    for (NSScreen* screen in NSScreen.screens) {
      // Cover the Dock (bottom by default; the inset handles side-docked layouts).
      // Same reversible contract as the menu mask: the windows vanish on exit.
      CGFloat inset = NSMinY(screen.visibleFrame) - NSMinY(screen.frame);
      CGFloat height = inset >= 40 && inset <= 160 ? inset : 80;
      NSRect strip = NSMakeRect(NSMinX(screen.frame), NSMinY(screen.frame),
                                NSWidth(screen.frame), height);
      NSWindow* mask = [[NSWindow alloc] initWithContentRect:strip
          styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
      mask.releasedWhenClosed = NO;
      mask.backgroundColor = kioskCover;
      mask.opaque = YES;
      mask.hasShadow = NO;
      mask.hidesOnDeactivate = NO;
      mask.ignoresMouseEvents = YES;
      mask.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                NSWindowCollectionBehaviorStationary;
      mask.level = CGWindowLevelForKey(kCGDockWindowLevelKey) + 1;
      [mask orderFront:nil];
      if (!gDockMasks) gDockMasks = [NSMutableArray new];
      [gDockMasks addObject:mask];
      NSNumber* display = screen.deviceDescription[@"NSScreenNumber"];
      fprintf(stderr, "[native-desktop] temporary dock mask window %ld level %ld display %u frame %.0f %.0f %.0f %.0f\n",
              (long)mask.windowNumber, (long)mask.level, display.unsignedIntValue,
              strip.origin.x, strip.origin.y, strip.size.width, strip.size.height);
    }
  }
  if (split_output_enabled()) {
    NSPoint origin = gWin.frame.origin;
    gFullOriginX = origin.x - ((CGFloat)w - (CGFloat)host_output_width(w)) * 0.5;
    gFullOriginY = origin.y - ((CGFloat)h - (CGFloat)host_output_height(h)) * 0.5;
    if (getenv("RDNATIVE_FIT_DISPLAY")) {
      NSScreen* target = nil;
      const char* requested = getenv("RDNATIVE_TARGET_DISPLAY_ID");
      unsigned long wanted = requested ? strtoul(requested, NULL, 10) : 0;
      for (NSScreen* screen in NSScreen.screens) {
        NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
        if (number && number.unsignedLongValue == wanted) { target = screen; break; }
      }
      if (target) {
        gFullOriginX = NSMinX(target.frame);
        gFullOriginY = NSMinY(target.frame);
      }
    }
    struct RDHostCrop crop = host_crop_snapshot(w, h);
    [gWin setFrameOrigin:NSMakePoint(gFullOriginX + crop.x,
                                    gFullOriginY + (CGFloat)h - crop.y - crop.h)];
    fprintf(stderr, "[native-split] host viewport %lu/%lu x %lu, left-aligned\n",
            (unsigned long)host_output_width(w), (unsigned long)w, (unsigned long)h);
  }
  open_native_child();

  gDelegate = [RDGpuWindowDelegate new];
  gWin.delegate = (id<NSWindowDelegate>)gDelegate;

  NSView* v = gWin.contentView;
  v.wantsLayer = YES;

  // ---- the CA path takes over the content layer entirely, or not at all ----------------------
  {
    CALayer* ca = (__bridge CALayer*)capres_make_layer();   // NULL unless WLSRV_CALAYER is set
    if (ca) {
      gCAPresLayer = ca;
      ca.frame = v.bounds;
      v.layer = ca;
      gWin.title = @"r-drift linux GPU (CALayer/IOSurface)";  // the arm names itself ON SCREEN
      size_layer(w, h);
      gWin.acceptsMouseMovedEvents = YES;
      ev_install_monitor();
      ev_selftest_arm();
      ev_selftest_mouse_arm();
      [gWin makeKeyAndOrderFront:nil];
      [NSApp activateIgnoringOtherApps:YES];
      fprintf(stderr, "[ca] window %lux%lu presenting through CALayer contents "
                      "(no CAMetalLayer, no drawable)\n",
              (unsigned long)w, (unsigned long)h);
      return 0;
    }
  }

  gLayer = [CAMetalLayer layer];
  gLayer.device          = dev;
  gLayer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
  gLayer.framebufferOnly = rdgpu_scanout_enabled() ? NO : YES;  // scanout reads drawable.texture.iosurface
  gLayer.opaque          = YES;
  gLayer.frame           = v.bounds;
  gLayer.drawableSize    = CGSizeMake((CGFloat)w, (CGFloat)h);
  v.layer = gLayer;

  // Mouse-moved events are off by default; the guest needs them for pointer motion.
  gWin.acceptsMouseMovedEvents = YES;
  ev_install_monitor();
  ev_selftest_arm();
  ev_selftest_mouse_arm();

  if (dynamic_regions_enabled()) [gWin orderOut:nil];
  else [gWin makeKeyAndOrderFront:nil];
  if (gNativeChild) {
    [gNativeChild makeKeyAndOrderFront:nil];
    [gNativeChild makeFirstResponder:gNativeChild.contentView];
  }
  [NSApp activateIgnoringOtherApps:YES];
  return 0;
}

// ---------------------------------------------------------------- gpuwin_open
long gpuwin_open(uint64_t w, uint64_t h) {
  if (w == 0 || h == 0 || w > 16384 || h > 16384) return -22;  // -EINVAL
  atomic_store(&gFbW, (int)w);
  atomic_store(&gFbH, (int)h);
  __block long rc = 0;
  run_on_main(^{ @autoreleasepool { rc = open_on_main((NSUInteger)w, (NSUInteger)h); } });
  return rc;
}

// ---------------------------------------------------------------- the shared present
// Take a drawable, blit `src` over it, present. Runs on whatever thread calls it: the only
// blocking call is -[CAMetalLayer nextDrawable], which is the display pacing itself.
// `done` (may be nil) fires when the GPU has finished reading `src`.
// Frames allowed in flight, i.e. how far the producer may run ahead of what the display has
// actually shown. 1 is strict single-buffering (draw, present, wait). RDGLINFLIGHT overrides.
static int inflight_limit(void) {
  static int v = 0;
  if (!v) {
    const char* e = getenv("RDGLINFLIGHT");
    v = e ? atoi(e) : 3;
    if (v < 1) v = 1;
    if (v > 3) v = 3;
  }
  return v;
}

static dispatch_semaphore_t inflight_sem(void) {
  static dispatch_semaphore_t s;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ s = dispatch_semaphore_create(inflight_limit()); });
  return s;
}

// RDGPU_SCANOUT_SELFCHECK=1 — after the compose+blit fence completes, sample a few pixels from
// the surface about to be handed to the Dock and log them, so a black scanout can be attributed
// to the surface content (vs. the present path) before the next run. CPU readback: gated off by
// default, and only the first few frames.
static void rdgpu_scanout_selfcheck(IOSurfaceRef surface) {
  static int enabled = -1;
  static unsigned frames;
  if (enabled < 0) enabled = getenv("RDGPU_SCANOUT_SELFCHECK") != NULL;
  if (!enabled || frames >= 12) return;
  frames++;
  if (!surface) {
    fprintf(stderr, "[rdgpu-scanout-selfcheck] frame=%u surface=NULL\n", frames - 1);
    return;
  }
  const size_t w = IOSurfaceGetWidth(surface), h = IOSurfaceGetHeight(surface);
  if (!w || !h) {
    fprintf(stderr, "[rdgpu-scanout-selfcheck] frame=%u surface %zux%zu (empty)\n", frames - 1, w, h);
    return;
  }
  if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) {
    fprintf(stderr, "[rdgpu-scanout-selfcheck] frame=%u lock failed\n", frames - 1);
    return;
  }
  const uint8_t* base = (const uint8_t*)IOSurfaceGetBaseAddress(surface);
  const size_t pitch = IOSurfaceGetBytesPerRow(surface);
  if (!base) {
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    fprintf(stderr, "[rdgpu-scanout-selfcheck] frame=%u no base address\n", frames - 1);
    return;
  }
  // Sample the center plus the four quadrants; a non-black verdict means real composited pixels
  // (the Hyprland desktop background is rgb(0b0e11), never uniformly black).
  const size_t xs[5] = { w/2, w/4, 3*w/4, w/4, 3*w/4 };
  const size_t ys[5] = { h/2, h/4, h/4, 3*h/4, 3*h/4 };
  int nonblack = 0, maxlum = 0;
  for (int i = 0; i < 5; ++i) {
    const uint8_t* p = base + ys[i] * pitch + xs[i] * 4;
    int lum = (int)p[0] + (int)p[1] + (int)p[2];
    if (lum > maxlum) maxlum = lum;
    if (lum > 24) nonblack = 1;
    fprintf(stderr, "[rdgpu-scanout-selfcheck] frame=%u sample(%zu,%zu) BGRA=%u,%u,%u,%u\n",
            frames - 1, xs[i], ys[i], p[0], p[1], p[2], p[3]);
  }
  IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
  fprintf(stderr, "[rdgpu-scanout-selfcheck] frame=%u %zux%zu maxlum=%d verdict=%s\n",
          frames - 1, w, h, maxlum, nonblack ? "NONBLACK" : "ALL_BLACK");
}

// `may_block`: whether the caller is willing to WAIT for a free slot.
//   NO  — the producer is generating frames faster than the display can show them and the newest
//         one always wins, so a frame that finds a present already in flight is simply dropped.
//         This is what keeps the producer from ever stalling; see the note in x11srv.c about a
//         blocked GL client bunching its clock reads and turning even delivery into a stutter.
//   YES — every frame is unique and slow to arrive (an ordinary X window), so dropping one
//         would lose content rather than a stale duplicate.
static long present_source(id<MTLTexture> src, id<MTLCommandQueue> q, int may_block,
                           dispatch_block_t done, uint64_t feedbackSequence) {
  CAMetalLayer* layer = gLayer;                    // assigned once, on main, before we get here
  if (!layer || !gPipe || !q) return -19;

  if (dynamic_regions_enabled()) {
    NSArray<RDRegionFrame*>* regions = region_frames_snapshot();
    if (!regions.count) { if (done) done(); return 0; }
    dispatch_semaphore_t inflight = inflight_sem();
    dispatch_time_t deadline = may_block ? dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC)
                                         : DISPATCH_TIME_NOW;
    if (dispatch_semaphore_wait(inflight, deadline) != 0) {
      if (!may_block) return -11;
      fprintf(stderr, "[native-region] present slot timed out; continuing\n");
    }
    id<MTLCommandBuffer> cb = [q commandBuffer];
    NSMutableArray* drawables = [NSMutableArray new];
    id<CAMetalDrawable> first = nil;
    for (RDRegionFrame* region in regions) {
      uint64_t drawableStart = getenv("RDNATIVE_PROFILE") ? mach_absolute_time() : 0;
      id<CAMetalDrawable> d = [region.layer nextDrawable];
      if (drawableStart) {
        static mach_timebase_info_data_t tb;
        static unsigned samples;
        if (!tb.denom) mach_timebase_info(&tb);
        if (samples++ < 512) fprintf(stderr, "[native-drawable] wait=%.3f id=0x%llx available=%d\n",
          (double)(mach_absolute_time()-drawableStart)*tb.numer/tb.denom/1.0e6,
          (unsigned long long)region.ident, d != nil);
      }
      if (!d) continue;
      if (!first) first = d;
      MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = d.texture;
      pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
      [enc setRenderPipelineState:gPipe];
      [enc setFragmentTexture:src atIndex:0];
      [enc setFragmentSamplerState:gSampler atIndex:0];
      struct RDHostCrop c = region.crop;
      float uv[4] = {(float)c.x / (float)src.width, (float)c.y / (float)src.height,
                     (float)c.w / (float)src.width, (float)c.h / (float)src.height};
      [enc setFragmentBytes:uv length:sizeof uv atIndex:0];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [enc endEncoding];
      [drawables addObject:d];
    }
    if (!first) {
      dispatch_semaphore_signal(inflight);
      if (feedbackSequence) rd_feedback_finish(0, feedbackSequence);
      return -11;
    }
    if (feedbackSequence) {
      [first addPresentedHandler:^(id<MTLDrawable> d) {
        (void)d; rd_feedback_finish(0, feedbackSequence);
      }];
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW,500*NSEC_PER_MSEC),
                     dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        rd_feedback_finish(0, feedbackSequence); // occlusion/drop recovery only
      });
    }
    // An occluded native-region window may never report presentedTime. Releasing
    // this slot on its presentation callback can stall every other region for
    // 500 ms. GPU completion is the correct reuse fence for the source texture;
    // each CAMetalLayer's drawable pool still applies display back-pressure.
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
      (void)b;
      dispatch_semaphore_signal(inflight);
      if (done) done();
    }];
    int hz = atomic_load(&gRefreshHz);
    for (id<CAMetalDrawable> d in drawables) {
      if (hz > 0 && !novsync() && !getenv("RDNATIVE_DEFAULT_PRESENT_SCHEDULE"))
        [cb presentDrawable:d afterMinimumDuration:1.0 / (double)hz];
      else [cb presentDrawable:d];
    }
    [cb commit];
    return 0;
  }

  dispatch_semaphore_t inflight = inflight_sem();
  uint64_t t0 = glstats_on() ? mach_absolute_time() : 0;
  // The blocking path waits, but never forever: a slot is handed back by the drawable's
  // presented handler, so were one ever to go missing an unbounded wait would freeze the window
  // outright. Going ahead without the slot instead just lets one extra frame be in flight.
  dispatch_time_t deadline = may_block ? dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC)
                                       : DISPATCH_TIME_NOW;
  if (dispatch_semaphore_wait(inflight, deadline) != 0) {
    if (!may_block) return -11;                    // -EAGAIN: superseded, drop it
    fprintf(stderr, "[gpuwin] present slot timed out; continuing\n");
  }
  id<CAMetalDrawable> drawable = [layer nextDrawable];
  if (t0) note_drawable_wait(mach_absolute_time() - t0);
  if (!drawable) {                                 // -EAGAIN: compositor starved us, skip frame
    dispatch_semaphore_signal(inflight);
    if (feedbackSequence) rd_feedback_finish(0, feedbackSequence);
    return -11;
  }
  // The slot is released when the frame REACHES THE DISPLAY, not when the GPU finishes with it:
  // that is what paces the next attempt to one per refresh without anyone blocking.
  [drawable addPresentedHandler:^(id<MTLDrawable> d) {
    if (t0) note_presented(d.presentedTime);
    dispatch_semaphore_signal(inflight);
    if (feedbackSequence && getenv("RDNATIVE_PRESENT_FEEDBACK_TRACE")) {
      static _Atomic unsigned traces;
      if (atomic_fetch_add(&traces, 1) < 128)
        fprintf(stderr, "[present-feedback] seq=%llu presented=%.6f\n",
                (unsigned long long)feedbackSequence, d.presentedTime);
    }
    if (feedbackSequence) rd_feedback_finish(0, feedbackSequence);
  }];
  if (feedbackSequence)
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,500*NSEC_PER_MSEC),
                   dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
      if (getenv("RDNATIVE_PRESENT_FEEDBACK_TRACE") &&
          rd_feedback_completed(0) < feedbackSequence)
        fprintf(stderr, "[present-feedback] watchdog seq=%llu\n",
                (unsigned long long)feedbackSequence);
      rd_feedback_finish(0, feedbackSequence); // occlusion/drop recovery only
    });

  // RD_PRESENT_CAPTURE=<path.ppm> — dump the guest framebuffer (src) to a PPM after
  // a few frames, so the full desktop (bar + windows) is captured even when the
  // metal-multi-scene direct path falls back to the CPU readback present path.
  {
    static int presentCaptured;
    static long presentFramesSeen;
    const char* presentCapturePath = getenv("RD_PRESENT_CAPTURE");
    const char* presentCapturePrefix = getenv("RD_PRESENT_CAPTURE_PREFIX");
    const char* presentSkip = getenv("RD_PRESENT_CAPTURE_SKIP");
    const char* presentEvery = getenv("RD_PRESENT_CAPTURE_EVERY");
    long skip = presentSkip ? atol(presentSkip) : 40;
    long every = presentEvery ? atol(presentEvery) : 1;
    ++presentFramesSeen;
    IOSurfaceRef io = src.iosurface;
    if (io && (presentCapturePath || presentCapturePrefix)) {
      char path[1024];
      int want = 0;
      if (presentCapturePrefix && (presentFramesSeen % every) == 0) {
        snprintf(path, sizeof path, "%s-%05ld.ppm", presentCapturePrefix, presentFramesSeen);
        want = 1;
      } else if (presentCapturePath && !presentCaptured && presentFramesSeen >= skip) {
        snprintf(path, sizeof path, "%s", presentCapturePath);
        want = 1;
        presentCaptured = 1;
      }
      if (want) {
        IOSurfaceLock(io, kIOSurfaceLockReadOnly, NULL);
        const uint8_t* base = (const uint8_t*)IOSurfaceGetBaseAddress(io);
        size_t pitch = IOSurfaceGetBytesPerRow(io);
        size_t w = IOSurfaceGetWidth(io), h = IOSurfaceGetHeight(io);
        uint8_t* buf = (uint8_t*)malloc(h * w * 3);
        if (base && buf) {
          // BGRA -> RGB in a single fast memcpy pass (no GPU readback, no per-pixel stdio)
          for (size_t y = 0; y < h; ++y) {
            const uint8_t* s = base + y * pitch;
            uint8_t* d = buf + y * w * 3;
            for (size_t x = 0; x < w; ++x) {
              d[x * 3 + 0] = s[x * 4 + 2];
              d[x * 3 + 1] = s[x * 4 + 1];
              d[x * 3 + 2] = s[x * 4 + 0];
            }
          }
          IOSurfaceUnlock(io, kIOSurfaceLockReadOnly, NULL);
          // write off the hot path so present never blocks on the filesystem
          char* ppath = strdup(path);
          size_t pw = w, ph = h;
          dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            FILE* f = fopen(ppath, "wb");
            if (f) {
              fprintf(f, "P6\n%zu %zu\n255\n", pw, ph);
              fwrite(buf, 1, ph * pw * 3, f);
              fclose(f);
            }
            free(buf);
            free(ppath);
          });
          fprintf(stderr, "[present-capture] queued %s %zux%zu\n", path, w, h);
        } else {
          IOSurfaceUnlock(io, kIOSurfaceLockReadOnly, NULL);
          free(buf);
        }
      }
    }
  }

  MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture     = drawable.texture;
  rp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;   // the blit covers every pixel
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;

  id<MTLCommandBuffer> cb = [q commandBuffer];
  id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
  [enc setRenderPipelineState:gPipe];
  [enc setFragmentTexture:src atIndex:0];
  [enc setFragmentSamplerState:gSampler atIndex:0];
  struct RDHostCrop crop = host_crop_snapshot(src.width, src.height);
  float uvRect[4] = {(float)crop.x / (float)src.width, (float)crop.y / (float)src.height,
                     (float)crop.w / (float)src.width, (float)crop.h / (float)src.height};
  [enc setFragmentBytes:uvRect length:sizeof uvRect atIndex:0];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [enc endEncoding];
  id<CAMetalDrawable> extraDrawable = nil;
  if (atomic_load(&gExtraCropVisible) && gExtraCropLayer) {
    struct RDHostCrop extra = extra_crop_snapshot(src.width, src.height);
    if (extra.w) extraDrawable = [gExtraCropLayer nextDrawable];
    if (extraDrawable) {
      MTLRenderPassDescriptor* erp = [MTLRenderPassDescriptor renderPassDescriptor];
      erp.colorAttachments[0].texture = extraDrawable.texture;
      erp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
      erp.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> eenc = [cb renderCommandEncoderWithDescriptor:erp];
      [eenc setRenderPipelineState:gPipe];
      [eenc setFragmentTexture:src atIndex:0];
      [eenc setFragmentSamplerState:gSampler atIndex:0];
      float extraUV[4] = {(float)extra.x / (float)src.width,
                          (float)extra.y / (float)src.height,
                          (float)extra.w / (float)src.width,
                          (float)extra.h / (float)src.height};
      [eenc setFragmentBytes:extraUV length:sizeof extraUV atIndex:0];
      [eenc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [eenc endEncoding];
    }
  }

  // RDGPU_SCANOUT=1: bypass WindowServer — hand the COMPOSITED frame's IOSurface straight to the
  // Dock's com.rdgpu.scanout service (CoreDisplay_Display_PresentIOSurface) instead of compositing
  // through the CAMetalLayer.
  //
  // The correct surface is `src.iosurface`, not `drawable.texture.iosurface`. On the Hyprland
  // path the composited desktop already lives in `src`: the guest's rd_surface_create() makes a
  // plain 'BGRA' IOSurface, rd_metal_scene_compose() renders the whole scene into it, and
  // rd_surface_present() wraps that same IOSurface as the Metal texture that arrives here as
  // `src`. The blit into the CAMetalLayer drawable below is only so WindowServer can also show
  // that frame; the drawable's own IOSurface is a display-optimized allocation whose bytes
  // CoreDisplay_Display_PresentIOSurface reads back black (it is NOT the same BGRA surface the
  // magenta self-test proved the present path accepts). src.iosurface IS that BGRA surface, so
  // handing it off is a zero-copy present of the actual frame.
  //
  // gpuwin_present_fmt's CPU-staging texture has no IOSurface (`src.iosurface == nil`); that arm
  // keeps the old drawable fallback unchanged. On any failure (no IOSurface at all, the OSAX not
  // installed, or the service not reachable) it falls through to presentDrawable unchanged.
  if (rdgpu_scanout_enabled()) {
    IOSurfaceRef scanSurface = src.iosurface ? src.iosurface : drawable.texture.iosurface;
    if (scanSurface) {
      // WindowServer fallback: still present via the CAMetalLayer so the display shows the
      // composited frame even when the direct scanout present doesn't land (the trampoline is
      // only driven by the CA idle loop, which is not always running). The direct handoff below
      // remains best-effort.
      int hz = atomic_load(&gRefreshHz);
      if (hz > 0 && !novsync())
        [cb presentDrawable:drawable afterMinimumDuration:1.0 / (double)hz];
      else
        [cb presentDrawable:drawable];
      if (extraDrawable) [cb presentDrawable:extraDrawable];
      // The compose that filled `src` (rd_metal_scene_compose) and this blit share one Metal
      // queue, so committing and waiting here orders BOTH and guarantees the surface bytes are
      // complete before the Dock reads them (only on this explicit scanout path — the normal
      // path below still never waits on the hot path).
      [cb commit];
      [cb waitUntilCompleted];
      rdgpu_scanout_selfcheck(scanSurface);
      if (done) done();
      dispatch_semaphore_signal(inflight);   // the presented handler we skip would have released it
      if (feedbackSequence) rd_feedback_finish(0, feedbackSequence);
      int sr = rdgpu_scanout_present(scanSurface, RDGPU_SCANOUT_ROLE_PRIMARY);
      return (sr == 0) ? 0 : sr;
    }
    static int saidNoSurf;
    if (!saidNoSurf) { saidNoSurf = 1;
      fprintf(stderr, "[rdgpu-scanout] neither src nor drawable texture exposes an IOSurface; "
                      "falling back to WindowServer present\n"); }
  }

  if (done) [cb addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; done(); }];

  // Ask for EVEN pacing, not just "as soon as possible". With several drawables in flight the
  // producer is free to bunch two frames together and then stall, and because a client like
  // glxgears derives its animation from the wall clock at draw time, two frames drawn 0.3 ms
  // apart but shown a refresh apart read as a stutter even though no frame was dropped. A
  // minimum duration of one refresh makes CoreAnimation space the presentations out and
  // back-pressures nextDrawable to match.
  int hz = atomic_load(&gRefreshHz);
  if (hz > 0 && !novsync())
    [cb presentDrawable:drawable afterMinimumDuration:1.0 / (double)hz];
  else
    [cb presentDrawable:drawable];
  if (extraDrawable) {
    if (hz > 0 && !novsync())
      [cb presentDrawable:extraDrawable afterMinimumDuration:1.0 / (double)hz];
    else
      [cb presentDrawable:extraDrawable];
  }
  [cb commit];                                     // never waitUntilCompleted on the hot path
  return 0;
}

// The thread that calls a present now paces the display: it blocks on nextDrawable and has to
// wake within a refresh. A default-QoS worker can be parked on an efficiency core, which shows
// up as a fat frame-time tail rather than as a lower average.
static void bump_present_thread_qos(void) {
  static _Atomic(int) done;
  int expected = 0;
  if (atomic_compare_exchange_strong(&done, &expected, 1))
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

// The content rect feeds gpuwin_cursor(); refreshing it is Cocoa work, so it is thrown at main
// asynchronously and only every so often rather than blocking a frame on it.
static void refresh_rect_occasionally(void) {
  static _Atomic(uint64_t) tick;
  if ((atomic_fetch_add(&tick, 1) % 16) == 0)
    dispatch_async(dispatch_get_main_queue(), ^{ @autoreleasepool { publish_rect(); } });
}

// ---------------------------------------------------------------- gpuwin_present
// The guest's RGBA8 CPU framebuffer. It has to be uploaded, so a staging texture is unavoidable
// — but the upload, the drawable and the blit all happen on the CALLING thread. This used to be
// one dispatch_sync to main per frame, which serialized the X server against the Cocoa run loop
// and put -[CAMetalLayer nextDrawable]'s wait for a refresh *on the main thread*.
//
// Three staging textures, cycled, with a semaphore released by the command buffer's completion
// handler: the CPU can fill frame N+1 while the GPU still reads frame N, but can never overwrite
// a texture that is still being sampled.
#define GPUWIN_STAGE_SLOTS 3
static id<MTLTexture>      gStage[GPUWIN_STAGE_SLOTS];
static IOSurfaceRef        gStageIO[GPUWIN_STAGE_SLOTS];
static NSUInteger          gStageW, gStageH;
static MTLPixelFormat      gStageFmt;
static int                 gStageIdx;
static dispatch_semaphore_t gStageSem;

// Touched only by the single thread that calls gpuwin_present().
//
// `fmt` is the CALLER's byte order, not a global setting. x11srv.c's framebuffer really is
// R,G,B,A in memory (x11srv.c:134) while a wl_shm buffer really is B,G,R,A, and the drawable is
// BGRA8Unorm either way — so telling Metal the truth about the source lets the sampler do the
// swizzle in hardware for free, and lets the Wayland compositor stop doing it in software. A
// format CHANGE reallocates, which only happens if both producers are live in one process.

// A plain 'BGRA' IOSurface matching the one rd_surface_create() makes on the Alpine/GL path.
// The scanout handoff (present_source) requires src.iosurface to be this kind of surface —
// the CAMetalLayer drawable's own IOSurface reads back BLACK, so we back the CPU staging
// texture with our own IOSurface so the composed frame is what gets handed to the scanout.
static IOSurfaceRef rd_stage_iosurface(uint32_t w, uint32_t h) {
  if (!w || !h) return NULL;
  const int64_t numvals[4] = { (int64_t)w, (int64_t)h, (int64_t)'BGRA', (int64_t)4 };
  const void* keys[4] = { kIOSurfaceWidth, kIOSurfaceHeight,
                          kIOSurfacePixelFormat, kIOSurfaceBytesPerElement };
  CFNumberRef nums[4]; const void* vals[4];
  for (int i = 0; i < 4; i++) {
    nums[i] = CFNumberCreate(NULL, kCFNumberSInt64Type, &numvals[i]); vals[i] = nums[i];
  }
  CFDictionaryRef props = CFDictionaryCreate(NULL, keys, vals, 4,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  IOSurfaceRef io = IOSurfaceCreate(props);
  for (int i = 0; i < 4; i++) if (nums[i]) CFRelease(nums[i]);
  if (props) CFRelease(props);
  return io;
}

static long ensure_stage(id<MTLDevice> dev, NSUInteger w, NSUInteger h, MTLPixelFormat fmt) {
  if (!gStageSem) gStageSem = dispatch_semaphore_create(GPUWIN_STAGE_SLOTS);
  if (gStage[0] && gStageW == w && gStageH == h && gStageFmt == fmt) return 0;
  MTLTextureDescriptor* td =
    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                       width:w
                                                      height:h
                                                   mipmapped:NO];
  td.usage       = MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModeShared;
  for (int i = 0; i < GPUWIN_STAGE_SLOTS; i++) {
    // Frames still in flight keep the old textures alive through their command buffers.
    IOSurfaceRef io = (fmt == MTLPixelFormatBGRA8Unorm)
                          ? rd_stage_iosurface((uint32_t)w, (uint32_t)h) : NULL;
    id<MTLTexture> tex = io ? [dev newTextureWithDescriptor:td iosurface:io plane:0]
                            : [dev newTextureWithDescriptor:td];
    if (!tex) { if (io) CFRelease(io); gStageW = gStageH = 0; return -12; }
    if (gStageIO[i]) CFRelease(gStageIO[i]);
    gStageIO[i] = io;                 // held for the texture's lifetime
    gStage[i] = tex;
  }
  gStageW = w; gStageH = h; gStageFmt = fmt; gStageIdx = 0;
  return 0;
}

long gpuwin_present(const uint8_t* rgba, uint64_t w, uint64_t h) {
  return gpuwin_present_fmt(rgba, w, h, /*bgra=*/0);
}

long gpuwin_present_fmt(const uint8_t* rgba, uint64_t w, uint64_t h, int bgra) {
  if (!rgba || w == 0 || h == 0 || w > 16384 || h > 16384) return -22;
  refresh_dynamic_regions((NSUInteger)w, (NSUInteger)h);
  refresh_split_width((NSUInteger)w, (NSUInteger)h);
  // On the CA path this window has no CAMetalLayer at all. Say so once and REFUSE, rather than
  // failing quietly with -ENODEV further down: a caller that reaches here has fallen back to the
  // Metal path without knowing it, and a run of the wrong arm reporting nothing is precisely the
  // shape of mis-attribution this project keeps catching.
  if (gCAPresLayer) {
    static int said;
    if (!said) { said = 1;
      fprintf(stderr, "[ca] REFUSED a Metal present: this window is on the CALayer path. "
                      "The frame did NOT reach the screen — fix the caller, do not read this "
                      "run as a CA measurement.\n"); }
    return -19;
  }
  atomic_store(&gFbW, (int)w);
  atomic_store(&gFbH, (int)h);

  if (!gLayer || !gPipe ||
      atomic_load(&gDrawableW) != (unsigned long)w ||
      atomic_load(&gDrawableH) != (unsigned long)h) {
    __block long rc = 0;
    run_on_main(^{ @autoreleasepool {
      rc = ensure_ready_on_main((NSUInteger)w, (NSUInteger)h);
    }});
    if (rc < 0) return rc;
  }

  bump_present_thread_qos();

  id<MTLDevice> dev = gpu_metal_device();
  id<MTLCommandQueue> q = gpu_metal_queue();
  if (!dev || !q) return -19;

  @autoreleasepool {
    long rc = ensure_stage(dev, (NSUInteger)w, (NSUInteger)h,
                           bgra ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm);
    if (rc < 0) return rc;

    dispatch_semaphore_wait(gStageSem, DISPATCH_TIME_FOREVER);
    id<MTLTexture> stage = gStage[gStageIdx];
    gStageIdx = (gStageIdx + 1) % GPUWIN_STAGE_SLOTS;
    [stage replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
             mipmapLevel:0
               withBytes:rgba
             bytesPerRow:(NSUInteger)w * 4];

    dispatch_semaphore_t sem = gStageSem;
    rc = present_source(stage, q, /*may_block=*/1, ^{ dispatch_semaphore_signal(sem); }, 0);
    if (rc < 0) dispatch_semaphore_signal(sem);    // nothing was queued; take the slot back
    if (rc < 0) return rc;
  }

  refresh_rect_occasionally();
  return 0;
}

// ---------------------------------------------------------------- gpuwin_present_texture
// Zero-copy present for the GL bridge: `mtl_texture` is the id<MTLTexture> glbridge just
// finished rendering into, so it goes straight to the drawable with a GPU blit — no readback,
// no CPU framebuffer, no waitUntilCompleted.
//
// `mtl_queue` MUST be the queue the frame was submitted on (glbridge's). Command buffers on one
// queue run in commit order, and this one is committed on the decode thread between frame N's
// commit and any of frame N+1's encoding, which is what keeps the blit reading finished pixels.
//
// Runs on the CALLING thread by design; see the file header.
long gpuwin_present_texture(void* mtl_texture, void* mtl_queue, uint64_t w, uint64_t h) {
  id<MTLTexture> tex = (__bridge id<MTLTexture>)mtl_texture;
  if (!tex || w == 0 || h == 0 || w > 16384 || h > 16384) return -22;
  refresh_dynamic_regions((NSUInteger)w, (NSUInteger)h);
  refresh_split_width((NSUInteger)w, (NSUInteger)h);
  // NOT IMPLEMENTED ON THE CA PATH, and this is a real gap rather than an oversight: the GL
  // bridge hands us a finished MTLTexture, and to put it on a CALayer it would have to have been
  // allocated -[MTLDevice newTextureWithDescriptor:iosurface:plane:] in the first place. That is
  // a change inside glbridge.m, not here, and it is outside the question this path exists to
  // answer (the Wayland shm client). x11srv's GL window keeps the Metal path.
  if (gCAPresLayer) {
    static int said;
    if (!said) { said = 1;
      fprintf(stderr, "[ca] gpuwin_present_texture is NOT supported on the CALayer path "
                      "(the GL bridge's texture is not IOSurface-backed). Frames dropped.\n"); }
    return -19;
  }
  atomic_store(&gFbW, (int)w);
  atomic_store(&gFbH, (int)h);

  // Cocoa/CALayer state only when it actually needs to change (first frame, resize).
  if (!gLayer || !gPipe ||
      atomic_load(&gDrawableW) != (unsigned long)w ||
      atomic_load(&gDrawableH) != (unsigned long)h) {
    __block long rc = 0;
    run_on_main(^{ @autoreleasepool {
      rc = ensure_ready_on_main((NSUInteger)w, (NSUInteger)h);
    }});
    if (rc < 0) return rc;
  }

  bump_present_thread_qos();

  long rc;
  uint64_t feedbackSequence = getenv("RDNATIVE_PRESENT_FEEDBACK") ? rd_feedback_submit(0) : 0;
  @autoreleasepool {
    // No staging semaphore here: glbridge already bounds itself to GLB_FRAMES_IN_FLIGHT, and
    // ordering against the next frame's writes comes from sharing its command queue.
    rc = present_source(tex, mtl_queue ? (__bridge id<MTLCommandQueue>)mtl_queue
                                       : gpu_metal_queue(), /*may_block=*/0, nil, feedbackSequence);
  }
  if (rc < 0 && feedbackSequence) rd_feedback_finish(0, feedbackSequence);
  if (rc < 0) return rc;
  if (getenv("RDNATIVE_DIRECT_GPU")) atomic_store(&gDirectGPUActive, 1);
  refresh_rect_occasionally();
  return 0;
}

// ---------------------------------------------------------------- gpuwin_poll
// Drains any pending NSEvents. Only useful when nobody is running -[NSApp run] (the test
// harness); under the real shim main is inside NSApp run and this is never called, so nothing
// here may be load-bearing.
int gpuwin_poll(void) {
  run_on_main(^{ @autoreleasepool {
    if (!NSApp) return;
    for (;;) {
      NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                      untilDate:[NSDate distantPast]
                                         inMode:NSDefaultRunLoopMode
                                        dequeue:YES];
      if (!e) break;
      [NSApp sendEvent:e];
    }
    [NSApp updateWindows];
  }});
  return atomic_load(&gClosed);
}


// ---------------------------------------------------------------- input events
// Cocoa delivers key/button events to the MAIN thread inside -[NSApp run]; the display server
// drains them from its own thread. A local NSEvent monitor is the only capture that works under
// a running NSApp without subclassing the view, and it sees everything routed to this app.
//
// Keycodes are translated to LINUX EVDEV values here, once, because that is what both Wayland
// (wl_keyboard.key) and X11 (keycode - 8) ultimately want, and doing it at the source keeps the
// protocol servers free of Carbon constants.
#define RDEV_Q 512
static struct gpuwin_event gEvQ[RDEV_Q];
static int                 gEvHead, gEvTail;            // head == tail  =>  empty
static pthread_mutex_t     gEvLk = PTHREAD_MUTEX_INITIALIZER;
static id                  gEvMonitor;

// RDINPUT_TRACE=1 prints every event that survives capture and translation, with the WINDOW-local
// coordinates it was tagged with. This is the choke point every input event passes through on its
// way to the display server, so a trace line here is proof the capture path saw it -- and the
// x,y printed is exactly what wl_pointer/X11 will be told. Without it, a silent input bug is
// indistinguishable from "the event never arrived".
static int gEvTrace = -1;
static const char* ev_type_name(uint32_t t) {
  switch (t) {
  case GPUWIN_EV_KEY:    return "KEY";
  case GPUWIN_EV_BUTTON: return "BUTTON";
  case GPUWIN_EV_MOTION: return "MOTION";
  case GPUWIN_EV_SCROLL: return "SCROLL";
  default:               return "?";
  }
}

static void ev_push(const struct gpuwin_event* e) {
  if (gEvTrace < 0) { const char* t = getenv("RDINPUT_TRACE"); gEvTrace = (t && *t && *t != '0'); }
  if (gEvTrace)
    fprintf(stderr, "[in] push %-6s code=0x%x state=%u at win(%d,%d) mods=0x%x\n",
            ev_type_name(e->type), e->code, e->state, e->x, e->y, e->mods);
  pthread_mutex_lock(&gEvLk);
  int next = (gEvHead + 1) % RDEV_Q;
  if (next != gEvTail) { gEvQ[gEvHead] = *e; gEvHead = next; }   // full: drop the newest
  pthread_mutex_unlock(&gEvLk);
}

int gpuwin_next_event(struct gpuwin_event* out) {
  int got = 0;
  pthread_mutex_lock(&gEvLk);
  if (gEvTail != gEvHead) { if (out) *out = gEvQ[gEvTail]; gEvTail = (gEvTail + 1) % RDEV_Q; got = 1; }
  pthread_mutex_unlock(&gEvLk);
  return got;
}

// macOS virtual keycode -> Linux evdev keycode. Indexed by the ANSI kVK_* value; 0 = unmapped.
// (Carbon's kVK_* constants are positional, so this is a plain lookup, not a switch.)
static const uint16_t kVK2EVDEV[128] = {
  [0x00] = 30,  [0x01] = 31,  [0x02] = 32,  [0x03] = 33,  [0x04] = 35,  [0x05] = 34,
  [0x06] = 44,  [0x07] = 45,  [0x08] = 46,  [0x09] = 47,  [0x0B] = 48,  [0x0C] = 16,
  [0x0D] = 17,  [0x0E] = 18,  [0x0F] = 19,  [0x10] = 21,  [0x11] = 20,  [0x12] = 2,
  [0x13] = 3,   [0x14] = 4,   [0x15] = 5,   [0x16] = 7,   [0x17] = 6,   [0x18] = 13,
  [0x19] = 10,  [0x1A] = 8,   [0x1B] = 12,  [0x1C] = 9,   [0x1D] = 11,  [0x1E] = 27,
  [0x1F] = 24,  [0x20] = 22,  [0x21] = 26,  [0x22] = 23,  [0x23] = 25,  [0x24] = 28,   // Return
  [0x25] = 38,  [0x26] = 36,  [0x27] = 40,  [0x28] = 37,  [0x29] = 39,  [0x2A] = 43,
  [0x2B] = 51,  [0x2C] = 53,  [0x2D] = 49,  [0x2E] = 50,  [0x2F] = 52,  [0x30] = 15,   // Tab
  [0x31] = 57,                                                                          // Space
  [0x32] = 41,  [0x33] = 14,                                                            // ` , BS
  [0x35] = 1,                                                                           // Escape
  [0x37] = 125, [0x38] = 42,  [0x39] = 58,  [0x3A] = 56,  [0x3B] = 29,  [0x3C] = 54,
  [0x3D] = 100, [0x3E] = 97,
  [0x7A] = 59,  [0x78] = 60,  [0x63] = 61,  [0x76] = 62,  [0x60] = 63,  [0x61] = 64,
  [0x62] = 65,  [0x64] = 66,  [0x65] = 67,  [0x6D] = 68,  [0x67] = 87,  [0x6F] = 88,
  [0x72] = 110, [0x73] = 102, [0x74] = 104, [0x75] = 111, [0x77] = 107, [0x79] = 109,
  [0x7B] = 105, [0x7C] = 106, [0x7D] = 108, [0x7E] = 103,
};

static int mac_shortcuts(void) {
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("RDMACKEYS") != NULL;
  return enabled;
}

static uint32_t ev_mods(NSEventModifierFlags f) {
  uint32_t m = 0;
  if (f & NSEventModifierFlagShift)   m |= GPUWIN_MOD_SHIFT;
  if (f & NSEventModifierFlagCapsLock) m |= GPUWIN_MOD_CAPS;
  if (f & NSEventModifierFlagControl) m |= mac_shortcuts() ? GPUWIN_MOD_LOGO : GPUWIN_MOD_CTRL;
  if (f & NSEventModifierFlagOption)  m |= GPUWIN_MOD_ALT;
  if (f & NSEventModifierFlagCommand) m |= mac_shortcuts() ? GPUWIN_MOD_CTRL : GPUWIN_MOD_LOGO;
  return m;
}

// Window-local, top-left origin — the same convention gpuwin_cursor() reports.
//
// Use the EVENT's own location, not +[NSEvent mouseLocation]: the latter is wherever the cursor
// happens to be right now, which is not where the event happened. (It also makes synthetic
// events report the physical cursor, which is how the self-test first showed coordinates far
// outside the window.) `locationInWindow` is bottom-left origin in points, and the layer's
// drawableSize matches the content rect 1:1, so no backing-scale conversion is needed.
static void ev_local_xy(NSEvent* e, int32_t* ox, int32_t* oy) {
  int extraWindow = e.window && e.window == gExtraCropWin;
  RDRegionFrame* dynamicRegion = nil;
  if (dynamic_regions_enabled() && e.window)
    for (RDRegionFrame* region in gRegionsByID.allValues)
      if (region.window == e.window) { dynamicRegion = region; break; }
  double rx, ry, rw, rh; int valid;
  pthread_mutex_lock(&gRectLk);
  rx = gRectX; ry = gRectY; rw = gRectW; rh = gRectH; valid = gRectValid;
  pthread_mutex_unlock(&gRectLk);
  if (!valid || rw <= 0 || rh <= 0) { *ox = 0; *oy = 0; return; }
  double lx, ly;
  NSPoint p = [e locationInWindow];
  if ([e window]) {                                // delivered to a window: p is window-base
    lx = p.x;
    ly = (extraWindow || dynamicRegion ? e.window.contentView.bounds.size.height : rh) - p.y;
  } else {
    // No window. -[NSEvent locationInWindow] is then documented to be in SCREEN coordinates, so
    // the event still knows where it happened — use it. The old code substituted
    // +[NSEvent mouseLocation] here, i.e. the LIVE cursor, which silently discarded the event's
    // own position: measured with a fabricated event aimed at content point (100,250),
    // ev_local_xy returned the cursor at (3529,217) instead. That is the exact "the click did
    // not land where I clicked" failure the comment above warns about, reintroduced in the
    // fallback branch. See src/test_pointer.m.
    lx = p.x - rx; ly = rh - (p.y - ry);
  }
  if (dynamicRegion) {
    CGFloat s = dynamicRegion.scale > 0.5 ? dynamicRegion.scale : 1.0;
    lx = lx * s + dynamicRegion.crop.x;
    ly = ly * s + dynamicRegion.crop.y;
  } else if (split_output_enabled()) {
    struct RDHostCrop crop = extraWindow ?
        extra_crop_snapshot(atomic_load(&gFbW), atomic_load(&gFbH)) :
        host_crop_snapshot(atomic_load(&gFbW), atomic_load(&gFbH));
    lx += crop.x; ly += crop.y;
  }
  if (lx < -32768) lx = -32768; else if (lx > 32767) lx = 32767;
  if (ly < -32768) ly = -32768; else if (ly > 32767) ly = 32767;
  *ox = (int32_t)lx; *oy = (int32_t)ly;
}

// Previous modifier state, so flagsChanged can be turned into discrete key up/down.
static NSEventModifierFlags gPrevFlags;

static void ev_handle(NSEvent* e) {
  // A detached Wayland toplevel has its own NSWindow and tagged input queue.
  // Never translate its window-local coordinates as if they belonged to gWin.
  if (dynamic_regions_enabled()) {
    int found = 0;
    for (RDRegionFrame* region in gRegionsByID.allValues)
      if (region.window == e.window) { found = 1; break; }
    if (!found) return;
  } else if ([e window] && [e window] != gWin && [e window] != gExtraCropWin) return;
  struct gpuwin_event g; memset(&g, 0, sizeof g);
  g.mods = ev_mods([e modifierFlags]);
  ev_local_xy(e, &g.x, &g.y);
  if (getenv("RDINPUT_SELFTEST_MOUSE_TRACE") &&
      ([e type] == NSEventTypeMouseMoved || [e type] == NSEventTypeLeftMouseDown ||
       [e type] == NSEventTypeLeftMouseUp))
    fprintf(stderr, "[in] routed event type=%ld window=%ld guest=(%d,%d)\n",
            (long)[e type], (long)e.window.windowNumber, g.x, g.y);

  switch ([e type]) {
  case NSEventTypeKeyDown:
  case NSEventTypeKeyUp: {
    unsigned short vk = [e keyCode];
    uint16_t ev = vk < 128 ? kVK2EVDEV[vk] : 0;
    if (!ev) return;
    g.type = GPUWIN_EV_KEY; g.code = ev;
    g.state = ([e type] == NSEventTypeKeyDown) ? 1 : 0;
    ev_push(&g);
    return;
  }
  case NSEventTypeFlagsChanged: {
    // Cocoa reports modifiers as a state change, Wayland wants press/release. Diff the mask.
    NSEventModifierFlags now = [e modifierFlags], was = gPrevFlags;
    gPrevFlags = now;
    static const struct { NSEventModifierFlags f; uint16_t ev; } kMod[] = {
      { NSEventModifierFlagShift, 42 }, { NSEventModifierFlagControl, 29 },
      { NSEventModifierFlagOption, 56 }, { NSEventModifierFlagCommand, 125 },
      { NSEventModifierFlagCapsLock, 58 },
    };
    for (unsigned i = 0; i < sizeof kMod / sizeof kMod[0]; i++) {
      int on = (now & kMod[i].f) != 0, before = (was & kMod[i].f) != 0;
      if (on == before) continue;
      g.type = GPUWIN_EV_KEY;
      g.code = mac_shortcuts() && kMod[i].f == NSEventModifierFlagCommand ? 29 :
               mac_shortcuts() && kMod[i].f == NSEventModifierFlagControl ? 125 : kMod[i].ev;
      g.state = (uint32_t)on;
      ev_push(&g);
    }
    return;
  }
  case NSEventTypeLeftMouseDown:  case NSEventTypeLeftMouseUp:
  case NSEventTypeRightMouseDown: case NSEventTypeRightMouseUp:
  case NSEventTypeOtherMouseDown: case NSEventTypeOtherMouseUp: {
    NSEventType t = [e type];
    int down = (t == NSEventTypeLeftMouseDown || t == NSEventTypeRightMouseDown ||
                t == NSEventTypeOtherMouseDown);
    uint32_t btn = 0x110;                                   // BTN_LEFT
    if (t == NSEventTypeRightMouseDown || t == NSEventTypeRightMouseUp) btn = 0x111;  // BTN_RIGHT
    if (t == NSEventTypeOtherMouseDown || t == NSEventTypeOtherMouseUp) btn = 0x112;  // BTN_MIDDLE
    g.type = GPUWIN_EV_BUTTON; g.code = btn; g.state = (uint32_t)down;
    ev_push(&g);
    return;
  }
  case NSEventTypeMouseMoved:
  case NSEventTypeLeftMouseDragged:
  case NSEventTypeRightMouseDragged:
  case NSEventTypeOtherMouseDragged:
    g.type = GPUWIN_EV_MOTION;
    ev_push(&g);
    return;
  case NSEventTypeScrollWheel: {
    double dy = [e scrollingDeltaY], dx = [e scrollingDeltaX];
    if (dx == 0 && dy == 0) return;
    g.type = GPUWIN_EV_SCROLL;
    g.x = (int32_t)(-dx); g.y = (int32_t)(-dy);             // Cocoa is "natural"; evdev is not
    ev_push(&g);
    return;
  }
  default:
    return;
  }
}

// Main thread only. Installed once, right after the window exists.
static void ev_install_monitor(void) {
  if (gEvMonitor) return;
  NSEventMask mask = NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged |
                     NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
                     NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
                     NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp |
                     NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
                     NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged |
                     NSEventMaskScrollWheel;
  gEvMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                     handler:^NSEvent*(NSEvent* e) {
    // Keep genuine native child-window events in AppKit. The guest event ring
    // only owns events targeting the Linux presentation window.
    if (gNativeChild && e.window == gNativeChild &&
        (e.type == NSEventTypeLeftMouseDown || e.type == NSEventTypeKeyDown))
      fprintf(stderr, "[native-child] AppKit event type=%ld window=%ld\n",
              (long)e.type, (long)e.window.windowNumber);
    if (gNativeChild && e.window == gNativeChild) return e;
    ev_handle(e);
    // Swallow key events: they are the guest's now, and letting AppKit also interpret them
    // produces the system beep for every keystroke the menu bar does not recognise.
    NSEventType t = [e type];
    if (t == NSEventTypeKeyDown || t == NSEventTypeKeyUp) return nil;
    return e;
  }];
}


// RDINPUT_SELFTEST=1 posts synthetic NSEvents into our own queue a moment after the window
// appears. They travel the REAL path — NSApp dispatch, the local monitor, ev_handle(), the ring,
// the display server — so a passing run proves the whole chain, and it needs no accessibility
// permission the way CGEventPost would.
static void ev_selftest_fire(void) { @autoreleasepool {
  if (!gWin) return;
  NSWindow* target = (getenv("RDINPUT_SELFTEST_EXTRA_CROP") && gExtraCropWin.isVisible) ?
      gExtraCropWin : gWin;
  if (dynamic_regions_enabled()) {
    NSArray<RDRegionFrame*>* regions = region_frames_snapshot();
    int index = getenv("RDINPUT_SELFTEST_DYNAMIC_REGION_INDEX") ?
        atoi(getenv("RDINPUT_SELFTEST_DYNAMIC_REGION_INDEX")) : 0;
    if (index < 0 || (NSUInteger)index >= regions.count) return;
    target = regions[(NSUInteger)index].window;
  }
  NSTimeInterval t = [[NSProcessInfo processInfo] systemUptime];
  // mouseEventWithType:location: takes WINDOW-local points (bottom-left origin) when a
  // windowNumber is given, not screen points.
  NSRect cr = [target contentRectForFrameRect:target.frame];
  NSPoint c = NSMakePoint(cr.size.width * 0.5, cr.size.height * 0.5);
  NSInteger wn = [target windowNumber];

  // Which keys to press. RDINPUT_SELFTEST_VK is a comma-separated list of macOS virtual
  // keycodes, so a test can drive a specific app (0x35 = Escape, 0x7D = Down, 0x24 = Return).
  // Default: 'a' (kVK_ANSI_A = 0x00 -> evdev 30) and Space (0x31 -> evdev 57).
  unsigned short vks[16] = { 0x00, 0x31 };
  const char*    chs[16] = { "a", " " };
  int nvk = 2;
  const char* spec = getenv("RDINPUT_SELFTEST_VK");
  if (spec && *spec) {
    nvk = 0;
    for (const char* q = spec; *q && nvk < 16; ) {
      vks[nvk] = (unsigned short)strtol(q, (char**)&q, 0);
      chs[nvk] = "";                       // characters are irrelevant; keyCode is what we read
      nvk++;
      while (*q == ',' || *q == ' ') q++;
    }
  }
  for (int i = 0; i < nvk; i++) {
    for (int down = 1; down >= 0; down--) {
      NSEvent* e = [NSEvent keyEventWithType:(down ? NSEventTypeKeyDown : NSEventTypeKeyUp)
                                    location:NSZeroPoint modifierFlags:0 timestamp:t
                                windowNumber:wn context:nil
                                  characters:[NSString stringWithUTF8String:chs[i]]
                 charactersIgnoringModifiers:[NSString stringWithUTF8String:chs[i]]
                                   isARepeat:NO keyCode:vks[i]];
      if (e) [NSApp postEvent:e atStart:NO];
    }
  }
  // Pointer motion, then a left click.
  for (int i = 0; i < 3; i++) {
    NSEvent* m = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                    location:NSMakePoint(c.x + i * 10, c.y)
                               modifierFlags:0 timestamp:t windowNumber:wn context:nil
                                 eventNumber:0 clickCount:0 pressure:0];
    if (m) [NSApp postEvent:m atStart:NO];
  }
  for (int down = 1; down >= 0; down--) {
    NSEvent* b = [NSEvent mouseEventWithType:(down ? NSEventTypeLeftMouseDown
                                                   : NSEventTypeLeftMouseUp)
                                    location:c modifierFlags:0 timestamp:t
                                windowNumber:wn context:nil eventNumber:0
                                  clickCount:1 pressure:(down ? 1.0 : 0.0)];
    if (b) [NSApp postEvent:b atStart:NO];
  }
  fprintf(stderr, "[gpu] input self-test: window %ld posted %d key, 3 motion, 2 button events\n",
          (long)wn, nvk * 2);
}}

static void ev_selftest_arm(void) {
  const char* e = getenv("RDINPUT_SELFTEST");
  if (!e || !*e || *e == '0') return;
  // A toolkit can take tens of seconds to get its first window up, so the delay has to be
  // settable: firing into a window that does not exist yet proves nothing.
  double delay = 2.5;
  const char* d = getenv("RDINPUT_SELFTEST_DELAY");
  if (d && *d) { double v = atof(d); if (v > 0) delay = v; }
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{ ev_selftest_fire(); });
}


// ------------------------------------------------------- scripted mouse self-test
// RDINPUT_SELFTEST=1 fires a fixed key burst and one centre click. That is a smoke test, not an
// instrument: it cannot aim at a particular widget, cannot separate press from release, and
// cannot be replayed. RDINPUT_SELFTEST_MOUSE is the instrument.
//
//   RDINPUT_SELFTEST_MOUSE="x,y[,action];x,y[,action];..."
//
// Coordinates are WINDOW-local, TOP-LEFT origin, in points -- the same convention gpuwin_cursor()
// reports, that ev_local_xy() produces, that wl_pointer surface coordinates use, and that a
// WLSRV_DUMP PPM is indexed by. One convention end to end, so a coordinate read off a dumped
// frame can be pasted straight into the script.
//
// Actions:  move (default) · down · up · click · rdown · rup · rclick · dclick
//           scroll:<dy>  (positive = content scrolls up, evdev sense)
//           wait         (burn one step interval without posting anything)
//
// The events are posted with -[NSApp postEvent:atStart:NO], so they enter the ordinary
// application event queue and are dispatched by the SAME -[NSApp sendEvent:] that dispatches a
// physical click. They are seen by the SAME local monitor installed in ev_install_monitor(), and
// translated by the SAME ev_handle()/ev_local_xy(). Nothing is written into the ring directly and
// nothing is handed to the compositor directly: a harness that injects below the layer under test
// returns a green result for a broken build. (This is also why CGEventPost is not used -- besides
// needing accessibility permission, it would move the user's real cursor.)
//
// THE POLL. gpuwin_cursor() normally reads +[NSEvent mouseLocation], the PHYSICAL cursor, and
// wlsrv's pump_pointer_poll() treats that as the source of truth for pointer position. Left alone
// it would drag the guest pointer back to wherever the physical mouse is sitting and overwrite
// every synthetic motion. So a script step also parks a VIRTUAL cursor at its own coordinate,
// which gpuwin_cursor() returns instead -- modelling a real user, whose physical cursor genuinely
// is where they just clicked. Set RDINPUT_SELFTEST_MOUSE_VCURSOR=0 to switch that off and let the
// poll keep reading the physical mouse; the two modes differing IS the poll-vs-event divergence,
// so the toggle is a measurement, not a workaround.
static _Atomic(int32_t) gVCurX, gVCurY;
static _Atomic(int)     gVCurOn;

enum { MS_MOVE, MS_DOWN, MS_DRAG, MS_UP, MS_CLICK, MS_RDOWN, MS_RUP, MS_RCLICK, MS_DCLICK,
       MS_SCROLL, MS_WAIT };
#define MS_MAX 64
struct ms_step { int32_t x, y; int action; int arg; };
static struct ms_step gMsStep[MS_MAX];
static int            gMsN, gMsI;
static double         gMsIntervalMs = 150.0;
static int            gMsUseVCursor = 1;

static const struct { const char* name; int action; } kMsActions[] = {
  { "move", MS_MOVE }, { "down", MS_DOWN }, { "drag", MS_DRAG }, { "up", MS_UP }, { "click", MS_CLICK },
  { "rdown", MS_RDOWN }, { "rup", MS_RUP }, { "rclick", MS_RCLICK }, { "dclick", MS_DCLICK },
  { "wait", MS_WAIT },
};

// Returns 0 on an unrecognised action so a typo fails loudly rather than silently becoming a move.
static int ms_parse_action(const char* s, size_t n, int* action, int* arg) {
  if (n == 0) { *action = MS_MOVE; *arg = 0; return 1; }
  if (n > 7 && strncmp(s, "scroll:", 7) == 0) {
    *action = MS_SCROLL; *arg = atoi(s + 7); return 1;
  }
  for (unsigned i = 0; i < sizeof kMsActions / sizeof kMsActions[0]; i++)
    if (strlen(kMsActions[i].name) == n && strncmp(s, kMsActions[i].name, n) == 0) {
      *action = kMsActions[i].action; *arg = 0; return 1;
    }
  return 0;
}

static int ms_parse(const char* spec) {
  gMsN = 0;
  for (const char* p = spec; *p && gMsN < MS_MAX; ) {
    while (*p == ' ' || *p == ';') p++;
    if (!*p) break;
    const char* end = strchr(p, ';');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    char* q = NULL;
    long x = strtol(p, &q, 0);
    if (q == p || *q != ',') { fprintf(stderr, "[in] mouse-script: bad step '%.*s'\n", (int)len, p);
                               return 0; }
    const char* r = q + 1;
    long y = strtol(r, &q, 0);
    if (q == r) { fprintf(stderr, "[in] mouse-script: bad step '%.*s'\n", (int)len, p); return 0; }

    const char* act = "";
    size_t actn = 0;
    if (*q == ',') {
      act = q + 1;
      actn = (size_t)((end ? end : p + len) - act);
      while (actn && (act[actn - 1] == ' ' || act[actn - 1] == '\t')) actn--;
    }
    int action, arg;
    if (!ms_parse_action(act, actn, &action, &arg)) {
      fprintf(stderr, "[in] mouse-script: unknown action '%.*s'\n", (int)actn, act);
      return 0;
    }
    gMsStep[gMsN].x = (int32_t)x; gMsStep[gMsN].y = (int32_t)y;
    gMsStep[gMsN].action = action; gMsStep[gMsN].arg = arg;
    gMsN++;
    if (!end) break;
    p = end + 1;
  }
  return gMsN;
}

// Main thread only. `ty` is TOP-LEFT window-local; Cocoa wants BOTTOM-LEFT, so it is flipped here
// against the same content-rect height ev_local_xy() flips back against.
static int ms_global_guest_point(void) {
  return getenv("RDINPUT_SELFTEST_DYNAMIC_GLOBAL") && dynamic_regions_enabled();
}

static RDRegionFrame* ms_target_region(int32_t x, int32_t y) {
  if (ms_global_guest_point()) {
    NSArray<RDRegionFrame*>* regions = region_frames_snapshot();
    for (RDRegionFrame* region in [regions reverseObjectEnumerator]) {
      struct RDHostCrop c = region.crop;
      if (x >= (int32_t)c.x && y >= (int32_t)c.y &&
          x < (int32_t)(c.x + c.w) && y < (int32_t)(c.y + c.h)) return region;
    }
    return nil;
  }
  const char* selected = getenv("RDINPUT_SELFTEST_DYNAMIC_REGION_INDEX");
  if (!selected || !dynamic_regions_enabled()) return nil;
  int index = atoi(selected);
  NSArray<RDRegionFrame*>* regions = region_frames_snapshot();
  if (index < 0 || (NSUInteger)index >= regions.count) return nil;
  return regions[(NSUInteger)index];
}

static void ms_post(NSEventType type, int32_t tx, int32_t ty, int clickCount, double dy) {
  if (!gWin) return;
  RDRegionFrame* region = ms_target_region(tx, ty);
  if (ms_global_guest_point() && !region) return;
  NSWindow* target = region.window ?: gWin;
  NSRect cr = [target contentRectForFrameRect:target.frame];
  NSPoint loc = NSMakePoint((CGFloat)(tx - (ms_global_guest_point() ? region.crop.x : 0)),
                            cr.size.height - (CGFloat)(ty - (ms_global_guest_point() ? region.crop.y : 0)));
  NSTimeInterval t = [[NSProcessInfo processInfo] systemUptime];
  NSInteger wn = [target windowNumber];
  NSEvent* e;
  if (type == NSEventTypeScrollWheel) {
    // -[NSEvent mouseEventWithType:] rejects scroll; a CGEvent converted to an NSEvent is the
    // only way to build one, and it still travels the normal queue once posted.
    CGEventRef cg = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, (int32_t)dy);
    if (!cg) return;
    e = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
  } else {
    e = [NSEvent mouseEventWithType:type location:loc modifierFlags:0 timestamp:t
                       windowNumber:wn context:nil eventNumber:0
                         clickCount:clickCount
                           pressure:(clickCount && type != NSEventTypeLeftMouseUp &&
                                     type != NSEventTypeRightMouseUp) ? 1.0 : 0.0];
  }
  if (e) [NSApp postEvent:e atStart:NO];
}

static void ms_fire_step(void);

static void ms_schedule_next(void) {
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(gMsIntervalMs * NSEC_PER_MSEC)),
                 dispatch_get_main_queue(), ^{ ms_fire_step(); });
}

static void ms_fire_step(void) { @autoreleasepool {
  if (gMsI >= gMsN) {
    fprintf(stderr, "[in] mouse-script: done (%d steps)\n", gMsN);
    return;
  }
  struct ms_step* s = &gMsStep[gMsI++];

  // Park the virtual cursor BEFORE posting, so a poll tick that lands between the move and the
  // button already agrees with the event.
  if (gMsUseVCursor) {
    RDRegionFrame* target = ms_target_region(s->x, s->y);
    atomic_store(&gVCurX, s->x + (!ms_global_guest_point() && target ? (int32_t)target.crop.x : 0));
    atomic_store(&gVCurY, s->y + (!ms_global_guest_point() && target ? (int32_t)target.crop.y : 0));
    atomic_store(&gVCurOn, 1);
  }

  switch (s->action) {
  case MS_WAIT:
    fprintf(stderr, "[in] mouse-script[%d]: wait\n", gMsI - 1);
    break;
  case MS_MOVE:
    fprintf(stderr, "[in] mouse-script[%d]: move (%d,%d)\n", gMsI - 1, s->x, s->y);
    ms_post(NSEventTypeMouseMoved, s->x, s->y, 0, 0);
    break;
  case MS_DOWN:
    fprintf(stderr, "[in] mouse-script[%d]: left-down (%d,%d)\n", gMsI - 1, s->x, s->y);
    ms_post(NSEventTypeLeftMouseDown, s->x, s->y, 1, 0);
    break;
  case MS_DRAG:
    fprintf(stderr, "[in] mouse-script[%d]: left-drag (%d,%d)\n", gMsI - 1, s->x, s->y);
    ms_post(NSEventTypeLeftMouseDragged, s->x, s->y, 1, 0);
    break;
  case MS_UP:
    fprintf(stderr, "[in] mouse-script[%d]: left-up (%d,%d)\n", gMsI - 1, s->x, s->y);
    ms_post(NSEventTypeLeftMouseUp, s->x, s->y, 1, 0);
    break;
  case MS_CLICK:
    // Move first, always. wl_pointer.button carries NO coordinates -- the client uses the
    // position from the last motion -- so a click posted without a preceding move lands wherever
    // the client last thought the pointer was.
    fprintf(stderr, "[in] mouse-script[%d]: click (%d,%d)\n", gMsI - 1, s->x, s->y);
    ms_post(NSEventTypeMouseMoved,    s->x, s->y, 0, 0);
    ms_post(NSEventTypeLeftMouseDown, s->x, s->y, 1, 0);
    ms_post(NSEventTypeLeftMouseUp,   s->x, s->y, 1, 0);
    break;
  case MS_DCLICK:
    fprintf(stderr, "[in] mouse-script[%d]: double-click (%d,%d)\n", gMsI - 1, s->x, s->y);
    ms_post(NSEventTypeMouseMoved,    s->x, s->y, 0, 0);
    ms_post(NSEventTypeLeftMouseDown, s->x, s->y, 1, 0);
    ms_post(NSEventTypeLeftMouseUp,   s->x, s->y, 1, 0);
    ms_post(NSEventTypeLeftMouseDown, s->x, s->y, 2, 0);
    ms_post(NSEventTypeLeftMouseUp,   s->x, s->y, 2, 0);
    break;
  case MS_RDOWN:
    fprintf(stderr, "[in] mouse-script[%d]: right-down (%d,%d)\n", gMsI - 1, s->x, s->y);
    ms_post(NSEventTypeRightMouseDown, s->x, s->y, 1, 0);
    break;
  case MS_RUP:
    fprintf(stderr, "[in] mouse-script[%d]: right-up (%d,%d)\n", gMsI - 1, s->x, s->y);
    ms_post(NSEventTypeRightMouseUp, s->x, s->y, 1, 0);
    break;
  case MS_RCLICK:
    fprintf(stderr, "[in] mouse-script[%d]: right-click (%d,%d)\n", gMsI - 1, s->x, s->y);
    ms_post(NSEventTypeMouseMoved,     s->x, s->y, 0, 0);
    ms_post(NSEventTypeRightMouseDown, s->x, s->y, 1, 0);
    ms_post(NSEventTypeRightMouseUp,   s->x, s->y, 1, 0);
    break;
  case MS_SCROLL:
    fprintf(stderr, "[in] mouse-script[%d]: scroll %d at (%d,%d)\n", gMsI - 1, s->arg, s->x, s->y);
    ms_post(NSEventTypeMouseMoved,  s->x, s->y, 0, 0);
    ms_post(NSEventTypeScrollWheel, s->x, s->y, 0, s->arg);
    break;
  }
  ms_schedule_next();
}}

static void ev_selftest_mouse_arm(void) {
  const char* spec = getenv("RDINPUT_SELFTEST_MOUSE");
  if (!spec || !*spec) return;
  if (!ms_parse(spec)) { fprintf(stderr, "[in] mouse-script: nothing to run\n"); return; }

  const char* v = getenv("RDINPUT_SELFTEST_MOUSE_VCURSOR");
  if (v && *v && *v == '0') gMsUseVCursor = 0;
  const char* iv = getenv("RDINPUT_SELFTEST_MOUSE_STEP_MS");
  if (iv && *iv) { double d = atof(iv); if (d > 0) gMsIntervalMs = d; }

  // Same reason the key self-test's delay is settable: GTK4 takes many seconds to raise its first
  // window, and clicking at a widget before the widget exists proves nothing.
  double delay = 3.0;
  const char* d = getenv("RDINPUT_SELFTEST_MOUSE_DELAY");
  if (!d || !*d) d = getenv("RDINPUT_SELFTEST_DELAY");
  if (d && *d) { double x = atof(d); if (x > 0) delay = x; }

  fprintf(stderr, "[in] mouse-script: %d step(s), delay %.2fs, interval %.0fms, vcursor %s\n",
          gMsN, delay, gMsIntervalMs, gMsUseVCursor ? "on" : "off (poll reads physical mouse)");
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{ ms_fire_step(); });
}

// ------------------------------------------------------------------ fake cursor path
// RDINPUT_FAKECURSOR="x,y;x,y;..." walks the cursor through a fixed path, wrapping. It exists so
// the INPUT BRIDGE (poll -> wl_pointer.motion -> client) can be tested end to end without a
// physical mouse, which is otherwise untestable headlessly.
//
// ONE STEP PER RDINPUT_FAKECURSOR_MS (default 16, one compositor poll tick) -- NOT one step per
// read. It used to advance on read, which made gpuwin_cursor() a getter that moved the thing it
// reported, and this function has THREE callers linked into shim_gui:
//
//   wlsrv.c        pump_pointer_poll()  ~60Hz, and only on ticks with no Cocoa motion
//   x11srv.c       QueryPointer         CLIENT-driven -- any xcb app advances it; xeyes hammers it
//   gpu_bridge.m   GPU_WIN_CURSOR       guest hypercall, guest-driven
//
// so the path advanced at a rate owned by unrelated code, and by whatever the guest happened to
// do. MEASURED on the identical construct in wlhead_stub.c, 3-point path sampled once per tick:
// one extra reader per tick traverses the path BACKWARDS, two extra readers freeze it completely
// and the harness reports a pinned cursor with zero motion -- a green light printed by a test
// that never ran. Reading is now pure; sample faster than the step period and the path is walked
// exactly in order no matter how many subsystems are looking at it.
//
// It also ANNOUNCES itself. It used to be completely silent, so "was RDINPUT_FAKECURSOR set in
// that run?" could not be answered from a log -- and answering it cost a whole investigation a
// morning. Anything that overrides the real cursor has to say so on stderr.
static int      gFakeNpts;
static int      gFakePts[64][2];
static uint64_t gFakeT0, gFakeStepNs = 16ull * 1000000ull;

static void fake_cursor_init_once(void) {
  const char* e = getenv("RDINPUT_FAKECURSOR");
  for (const char* p = e; p && *p && gFakeNpts < 64; ) {
    const char* comma = strchr(p, ',');
    if (!comma) break;
    gFakePts[gFakeNpts][0] = atoi(p);
    gFakePts[gFakeNpts][1] = atoi(comma + 1);
    gFakeNpts++;
    const char* semi = strchr(comma, ';');
    if (!semi) break;
    p = semi + 1;
  }
  const char* ms = getenv("RDINPUT_FAKECURSOR_MS");
  if (ms && *ms) { double d = atof(ms); if (d > 0) gFakeStepNs = (uint64_t)(d * 1e6); }
  if (gFakeNpts) {
    gFakeT0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    fprintf(stderr, "[in] FAKE CURSOR ACTIVE: %d point(s), one step per %.0f ms — the poll "
                    "reports this path, NOT the physical mouse and NOT the self-test's virtual "
                    "cursor\n", gFakeNpts, (double)gFakeStepNs / 1e6);
  }
}

// ---------------------------------------------------------------- gpuwin_cursor
// Live, lock-light, and independent of poll. +[NSEvent mouseLocation] is thread-safe and
// returns screen points with a BOTTOM-left origin; X11 wants window-local TOP-left.
// ---------------------------------------------------------------- macOS window identity
// The Linux client already tells us who it is: xdg_toplevel.set_title and .set_app_id arrive at
// the compositor on every real toolkit. Until now both were logged and discarded, and the window
// was hardcoded to "r-drift linux GPU" — so Blender, GTK and Qt apps were indistinguishable in
// the title bar, in the Dock, and to any window manager looking at them.
//
// Titles arrive on the compositor thread; NSWindow is main-thread-only, hence run_on_main.
void gpuwin_set_title(const char* utf8) {
  if (!utf8 || !*utf8) return;
  NSString* t = [NSString stringWithUTF8String:utf8];
  if (!t) return;
  if (getenv("RDVERBOSE")) fprintf(stderr, "[win] title <- %s\n", utf8);
  run_on_main(^{ @autoreleasepool { gWantedTitle = t; if (gWin) gWin.title = t; } });
}

// The Dock tile and the menu-bar name of a non-bundled process come from the PROCESS name, so
// setting that is what makes a Linux app stop calling itself "shim_gui".
void gpuwin_set_appname(const char* utf8) {
  if (!utf8 || !*utf8) return;
  NSString* n = [NSString stringWithUTF8String:utf8];
  if (!n) return;
  if (getenv("RDVERBOSE")) fprintf(stderr, "[win] app_id <- %s\n", utf8);
  run_on_main(^{ @autoreleasepool {
    [[NSProcessInfo processInfo] setProcessName:n];
  } });
}

// xdg_toplevel.set_minimized — the Linux app asking to be minimised should minimise the real
// window, not be ignored.
void gpuwin_minimize(void) {
  run_on_main(^{ @autoreleasepool { if (gWin) [gWin miniaturize:nil]; } });
}

// Clipboard callbacks are defined by the named-board-only fixture module.

// Guest pixels per host point for the PRIMARY (non-region) window. The full-screen fit
// path scales the WHOLE framebuffer to the fitted display's physical pixels, so one host
// point equals backingScaleFactor guest pixels there; every other primary-window layout
// keeps drawableSize == content rect 1:1, so the factor is 1.0.
static CGFloat primary_window_scale(void) {
  if (fit_display_enabled() && gFitDisplayScale > 0.5) return gFitDisplayScale;
  return 1.0;
}

static int cursor_debug_enabled(void) {
  static int v = -1;
  if (v < 0) v = getenv("RDGPU_CURSOR_DEBUG") != NULL;
  return v;
}

// ~10 Hz throttle so the 60 Hz pointer poll does not flood stderr when debugging.
static int cursor_debug_due(void) {
  static uint64_t last;
  uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
  if (now - last < 100000000ULL) return 0;
  last = now;
  return 1;
}

void gpuwin_cursor(int* x, int* y) {
  static pthread_once_t fake_once = PTHREAD_ONCE_INIT;   // three threads reach this
  pthread_once(&fake_once, fake_cursor_init_once);
  if (gFakeNpts) {
    uint64_t dt = clock_gettime_nsec_np(CLOCK_MONOTONIC) - gFakeT0;
    uint64_t i  = (dt / gFakeStepNs) % (uint64_t)gFakeNpts;
    if (x) *x = gFakePts[i][0];
    if (y) *y = gFakePts[i][1];
    return;
  }
  // The scripted mouse self-test parks a virtual cursor at each step. While it is armed the poll
  // must report THAT, not the physical mouse -- otherwise pump_pointer_poll() drags the guest
  // pointer back to wherever the real cursor is sitting and every synthetic motion is undone.
  // See ev_selftest_mouse_arm(); RDINPUT_SELFTEST_MOUSE_VCURSOR=0 leaves this off on purpose.
  if (atomic_load(&gVCurOn)) {
    if (dynamic_regions_enabled()) {
      if (x) *x = atomic_load(&gVCurX);
      if (y) *y = atomic_load(&gVCurY);
      return;
    }
    struct RDHostCrop crop = host_crop_snapshot(atomic_load(&gFbW), atomic_load(&gFbH));
    if (x) *x = atomic_load(&gVCurX) + crop.x;
    if (y) *y = atomic_load(&gVCurY) + crop.y;
    return;
  }

  int fbw = atomic_load(&gFbW), fbh = atomic_load(&gFbH);

  double rx, ry, rw, rh; int valid;
  double ex, ey, ew, eh;
  pthread_mutex_lock(&gRectLk);
  rx = gRectX; ry = gRectY; rw = gRectW; rh = gRectH; valid = gRectValid;
  ex = gExtraRectX; ey = gExtraRectY; ew = gExtraRectW; eh = gExtraRectH;
  pthread_mutex_unlock(&gRectLk);

  if (!valid || rw <= 0 || rh <= 0) {              // before the first present: window centre
    if (x) *x = fbw > 0 ? fbw / 2 : 0;
    if (y) *y = fbh > 0 ? fbh / 2 : 0;
    return;
  }

  NSPoint m = [NSEvent mouseLocation];
  if (dynamic_regions_enabled()) {
    NSArray<RDRegionFrame*>* regions = region_frames_snapshot();
    for (RDRegionFrame* region in [regions reverseObjectEnumerator]) {
      NSRect frame = region.screenFrame;
      if (!NSPointInRect(m, frame)) continue;
      CGFloat s = region.scale > 0.5 ? region.scale : 1.0;
      int gx = (int)((m.x - frame.origin.x) * s) + (int)region.crop.x;
      int gy = (int)((frame.size.height - (m.y - frame.origin.y)) * s) + (int)region.crop.y;
      if (cursor_debug_enabled() && cursor_debug_due())
        fprintf(stderr, "[cursor] region mouse=(%.1f,%.1f) id=0x%llx "
                "frame=(%.0f,%.0f %.0fx%.0f) crop=(%u,%u %ux%u) scale=%.2f "
                "ratio(crop.w/frame.w)=%.2f guest=(%d,%d)\n",
                m.x, m.y, (unsigned long long)region.ident,
                frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
                region.crop.x, region.crop.y, region.crop.w, region.crop.h,
                s, frame.size.width > 0 ? (CGFloat)region.crop.w / frame.size.width : 0.0,
                gx, gy);
      if (x) *x = gx;
      if (y) *y = gy;
      return;
    }
    if (x) *x = -1;
    if (y) *y = -1;
    return;
  }
  if (atomic_load(&gExtraCropVisible) && ew > 0 && eh > 0 &&
      m.x >= ex && m.x < ex + ew && m.y >= ey && m.y < ey + eh) {
    struct RDHostCrop extra = extra_crop_snapshot(fbw, fbh);
    if (extra.w) {
      if (x) *x = (int)(m.x - ex) + extra.x;
      if (y) *y = (int)(eh - (m.y - ey)) + extra.y;
      return;
    }
  }
  CGFloat ps = primary_window_scale();
  double lx = (m.x - rx) * ps;
  double ly = (rh - (m.y - ry)) * ps;              // flip to top-left origin, points -> pixels
  if (split_output_enabled()) {
    struct RDHostCrop crop = host_crop_snapshot(fbw, fbh);
    lx += crop.x; ly += crop.y;
  }
  if (cursor_debug_enabled() && cursor_debug_due())
    fprintf(stderr, "[cursor] primary mouse=(%.1f,%.1f) rect=(%.0f,%.0f %.0fx%.0f) "
            "scale=%.2f fbw/rw=%.2f guest=(%.1f,%.1f)\n",
            m.x, m.y, rx, ry, rw, rh, ps, rw > 0 ? (double)fbw / rw : 0.0, lx, ly);

  // DO NOT clamp to the content rect. X11 root_x/root_y are SIGNED 16-bit, so coordinates
  // outside the window (and negative ones) are legal and expected. Clamping made every cursor
  // position outside our small window collapse to one constant value, so a client polling
  // QueryPointer concluded the pointer had STOPPED. xeyes then rides its own idle backoff
  // ({50,100,200,400} ms, a 750 ms ramp) and appears to freeze for ~755 ms at a time — measured
  // p99 402 ms with the cursor outside the window vs 55.6 ms inside. Clamp only to the signed
  // 16-bit wire range so the values still encode correctly.
  if (lx < -32768) lx = -32768; else if (lx > 32767) lx = 32767;
  if (ly < -32768) ly = -32768; else if (ly > 32767) ly = 32767;

  if (x) *x = (int)lx;
  if (y) *y = (int)ly;
}
