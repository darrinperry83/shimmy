// chooser.c — the shimmy boot chooser (early launchd daemon, runs BEFORE WindowServer).
//
// Two-option key menu with a 10-second countdown that defaults to macOS:
//
//        shimmy
//    [1] Boot macOS
//    [2] Boot shimmy (Linux)
//    (defaults to macOS in 10 s)
//
// The menu is drawn ON SCREEN before WindowServer exists: the text is rendered with an
// embedded 8x13 font (chooser_font.h) into an RGBA8 framebuffer, and the framebuffer is
// presented straight to the display via the private CoreDisplay_Display_PresentIOSurface
// (the same WindowServer-free scanout present the shim's scanout path uses) — no WindowServer
// to composite to. The keyboard is read from the HID stack (IOHIDManager / IOKit) with a
// /dev/console (fd 0) fallback.
//
// Branches:
//   macOS  — the NORMAL path. The chooser exits 0 and launchd's normal boot continues;
//            loginwindow starts the real WindowServer on demand. Nothing is registered.
//            (An optional --kickstart-windowserver makes the "launch the real WindowServer"
//             step explicit, but it is off by default because the normal path already does it.)
//   shimmy — LAUNCH the windowserver stand-in (shim_windowserver) as a CHILD with
//            RDNATIVE_WINDOWSERVER_SYSTEM=1. The stand-in — not the chooser — performs the
//            real check-in (bootstrap_check_in "com.apple.windowserver" then
//            "com.apple.windowserver.active", lowercase, that order, in the system bootstrap
//            domain) and runs the ~37-function CGS/SLS server. The child inherits the
//            chooser's task bootstrap port (the launchd-pre-created names), so the check-in
//            resolves exactly as if the chooser itself did it. The chooser (parent) stays
//            alive as the launchd job's primary process and WAITS. On a clean stand-in exit
//            — or a "macOS" key on fd 0, which SIGTERMs the stand-in — the chooser
//            WARM-SWITCHES: the stand-in's exit releases the receive rights it checked in
//            (the names go dead), and the chooser relaunches the real WindowServer
//            (branch_macos). Any crash also falls back to macOS.
//
// SAFETY:
//   * macOS is always the default — a timeout, EOF, or any error boots macOS.
//   * --dry-run prints the full plan and touches NOTHING (never forks, never kickstarts).
//   * The live present is OPT-IN (SHIMMY_PRESENT=1 / --present). A plain `./chooser` run
//     renders to memory and reads keys but NEVER flips the live display, so the headless
//     validation can prove the render + key branch without touching the screen.
//   * SHIMMY_WINDOWSERVER_SYSTEM=0 runs the stand-in in its *test-name* mode
//     (bootstrap_register com.shimmy.windowserver.test), so the launch + server +
//     warm-switch handback can be proven headlessly WITHOUT the real com.apple.windowserver
//     or the system domain.
//   * The actual `launchctl kickstart` of the real WindowServer is OPT-IN
//     (SHIMMY_WARM_SWITCH_KICKSTART=1). Without it the warm-switch prints the command and
//     exits. Running the binary by hand therefore cannot restart the live WindowServer,
//     and even in system mode the stand-in refuses to check in the real name unless root
//     under the launchd job that owns it.
//   * --checkin <name> <seconds> is a test hook: it checks in ONE arbitrary name (used with a
//     scratch launchd job in the caller's own gui domain) so the exact check-in code path can
//     be exercised headlessly without touching com.apple.windowserver or the system domain.

#include <servers/bootstrap.h>
#include <mach/mach.h>
#include <mach/task_special_ports.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>
#include <setjmp.h>

// On-screen present (CoreDisplay via dlopen) + HID keyboard (IOHIDManager) + the IOSurface the
// present writes into. CoreDisplay is NOT linked (it is not a linkable dylib on every Xcode
// version); every private symbol is dlsym'd by exact name.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOSurface/IOSurface.h>
#include <CoreGraphics/CoreGraphics.h>

#include "chooser_font.h"

#define SVC_MAIN   "com.apple.windowserver"
#define SVC_ACTIVE "com.apple.windowserver.active"
#define TIMEOUT_DEFAULT 10
#define SHIMMY_WINDOWSERVER_DEFAULT_PATH "/usr/local/sbin/shim_windowserver"

// Fallback framebuffer size for headless runs (present off); the live present uses the display.
#define MENU_DEFAULT_W 960
#define MENU_DEFAULT_H 540

typedef enum { BRANCH_MACOS = 0, BRANCH_SHIMMY = 1 } branch_t;

static const char *branch_name(branch_t b) {
  return b == BRANCH_SHIMMY ? "shimmy" : "macOS";
}

static void usage(FILE *f) {
  fprintf(f,
    "usage: chooser [--dry-run] [--choice macOS|shimmy] [--kickstart-windowserver]\n"
    "               [--present] [--probe-present] [--probe-hid]\n"
    "               [--render <path.ppm>] [--render-size WxH]\n"
    "               [--checkin <name> [seconds]]\n"
    "  --dry-run               print the chosen branch + full plan, touch nothing\n"
    "  --choice <branch>       force a branch (headless testing); macOS is the fallback\n"
    "  --kickstart-windowserver  on the macOS branch, kickstart system/com.apple.WindowServer\n"
    "  --present               enable the live direct-scanout present (equiv SHIMMY_PRESENT=1)\n"
    "  --probe-present         READ-ONLY: dlsym the CoreDisplay present symbols + list displays\n"
    "  --probe-hid             READ-ONLY: open IOHIDManager and enumerate keyboards\n"
    "  --render <path.ppm>     render the menu to a PPM (captured framebuffer) and exit\n"
    "  --render-size WxH       framebuffer size for --render (default %dx%d)\n"
    "  --checkin <name> [s]    test hook: bootstrap_check_in ONE name, hold it `s` seconds\n"
    "env: SHIMMY_BOOT_TIMEOUT (default 10), SHIMMY_BOOT_CHOICE, SHIMMY_PRESENT (0/1)\n"
    "     SHIMMY_WINDOWSERVER_CMD     stand-in path (default %s)\n"
    "     SHIMMY_WINDOWSERVER_SYSTEM  1 (default) real check-in; 0 = test names (safe)\n"
    "     SHIMMY_WARM_SWITCH_KICKSTART 0 (default) print only; 1 = really kickstart\n",
    MENU_DEFAULT_W, MENU_DEFAULT_H, SHIMMY_WINDOWSERVER_DEFAULT_PATH);
}

static int boot_timeout(void) {
  const char *v = getenv("SHIMMY_BOOT_TIMEOUT");
  if (v && *v) {
    char *end = NULL;
    long t = strtol(v, &end, 10);
    if (end && *end == '\0' && t > 0) return (int)t;
  }
  return TIMEOUT_DEFAULT;
}

// Poll fd 0 for a key, in milliseconds; returns the byte, or -1 on timeout/EOF.
static int poll_key(int timeout_ms) {
  struct pollfd pfd;
  pfd.fd = 0;
  pfd.events = POLLIN;
  pfd.revents = 0;
  int r = poll(&pfd, 1, timeout_ms);
  if (r <= 0) return -1;
  unsigned char c = 0;
  ssize_t n = read(0, &c, 1);
  if (n != 1) return -1;
  return (int)c;
}

// Any unrecognised key is macOS — the safe default is the whole point of the chooser.
static branch_t key_to_branch(int key) {
  switch (key) {
    case '2': case 's': case 'S': case 'l': case 'L': return BRANCH_SHIMMY;
    case '1': case 'm': case 'M': case '0': case '\n': case '\r':
    default:                                    return BRANCH_MACOS;
  }
}

// The warm-switch key is an EXPLICIT "back to macOS" key only. '\n'/'\r' are deliberately
// excluded: the menu read above consumes one byte, so a leftover newline from "2<enter>" would
// otherwise be misread as an immediate warm-switch.
static int is_warm_switch_key(int key) {
  switch (key) {
    case '1': case 'm': case 'M': case '0': return 1;
    default:                                return 0;
  }
}

static branch_t forced_branch(const char *forced) {
  return strcmp(forced, "shimmy") == 0 ? BRANCH_SHIMMY : BRANCH_MACOS;
}

// ---------------------------------------------------------------------------------------------
// On-screen menu: render an RGBA8 framebuffer with the embedded 8x13 font.
// ---------------------------------------------------------------------------------------------

typedef struct { uint8_t r, g, b; } chooser_rgb;

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Integer scale for the 8x16 cell, derived from the framebuffer width so the menu is readable
// on a 1800px builtin or a 3440px external without being tiny on a test buffer.
static int menu_scale(int w) {
  if (w >= 2560) return 4;
  if (w >= 1440) return 3;
  return 2;
}

static void fb_fill(uint8_t *fb, int w, int h, chooser_rgb c) {
  for (int y = 0; y < h; y++) {
    uint8_t *p = fb + (size_t)y * (size_t)w * 4;
    for (int x = 0; x < w; x++) {
      p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 0xff;
      p += 4;
    }
  }
}

static const uint8_t *glyph_for(unsigned char c) {
  if (c < CHOOSER_FONT_FIRST || c > CHOOSER_FONT_LAST) c = '?';
  return chooser_font8x13[c - CHOOSER_FONT_FIRST];
}

static int text_width_px(const char *s, int scale) {
  return (int)strlen(s) * CHOOSER_FONT_W * scale;
}

static void draw_glyph(uint8_t *fb, int w, int h, int x0, int y0, int scale,
                       unsigned char c, chooser_rgb fg) {
  const uint8_t *g = glyph_for(c);
  for (int gy = 0; gy < CHOOSER_FONT_H; gy++) {
    uint8_t bits = g[gy];
    for (int gx = 0; gx < CHOOSER_FONT_W; gx++) {
      if (!(bits & (0x80 >> gx))) continue;
      for (int sy = 0; sy < scale; sy++) {
        int y = y0 + gy * scale + sy;
        if (y < 0 || y >= h) continue;
        uint8_t *row = fb + (size_t)y * (size_t)w * 4;
        for (int sx = 0; sx < scale; sx++) {
          int x = x0 + gx * scale + sx;
          if (x < 0 || x >= w) continue;
          uint8_t *p = row + (size_t)x * 4;
          p[0] = fg.r; p[1] = fg.g; p[2] = fg.b; p[3] = 0xff;
        }
      }
    }
  }
}

// Draw `s` horizontally centred on `cx`, with the cell's top-left at `y`.
static void draw_text(uint8_t *fb, int w, int h, int cx, int y, int scale,
                      const char *s, chooser_rgb fg) {
  int x = cx - text_width_px(s, scale) / 2;
  for (size_t i = 0; s[i]; i++) {
    draw_glyph(fb, w, h, x, y, scale, (unsigned char)s[i], fg);
    x += CHOOSER_FONT_W * scale;
  }
}

// The menu. 5 lines (one blank), centred, black background, VGA palette colours.
static void render_menu(uint8_t *fb, int w, int h, int scale, int countdown) {
  static const chooser_rgb white = {255, 255, 255};
  static const chooser_rgb green = { 85, 255,  85};
  static const chooser_rgb gray  = {170, 170, 170};
  chooser_rgb black = {0, 0, 0};
  fb_fill(fb, w, h, black);

  int cell = CHOOSER_CELL_H * scale;
  int lines = 5;
  int start_y = (h - lines * cell) / 2;
  if (start_y < 0) start_y = 0;
  int cx = w / 2;
  char cd[64];
  snprintf(cd, sizeof cd, "(defaults to macOS in %d s)", countdown);

  draw_text(fb, w, h, cx, start_y + 0 * cell, scale, "shimmy", white);
  draw_text(fb, w, h, cx, start_y + 1 * cell, scale, "[1] Boot macOS", white);
  draw_text(fb, w, h, cx, start_y + 2 * cell, scale, "[2] Boot shimmy (Linux)", green);
  draw_text(fb, w, h, cx, start_y + 4 * cell, scale, cd, gray);
}

static size_t count_nonblank(const uint8_t *fb, int w, int h) {
  size_t n = 0;
  for (int y = 0; y < h; y++) {
    const uint8_t *p = fb + (size_t)y * (size_t)w * 4;
    for (int x = 0; x < w; x++) {
      if (p[0] || p[1] || p[2]) n++;
      p += 4;
    }
  }
  return n;
}

static int write_ppm(const char *path, const uint8_t *fb, int w, int h) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = 0; y < h; y++) {
    const uint8_t *p = fb + (size_t)y * (size_t)w * 4;
    for (int x = 0; x < w; x++) { fputc(p[0], f); fputc(p[1], f); fputc(p[2], f); p += 4; }
  }
  fclose(f);
  return 0;
}

// ---------------------------------------------------------------------------------------------
// Direct present: dlopen CoreDisplay, map the display id -> CoreDisplayRef, write the framebuffer
// into an IOSurface, and CoreDisplay_Display_PresentIOSurface it to the scanout. WindowServer-free.
//
// The private CGX hop can fault (branch through a NULL function pointer) when the process has no
// live connection to com.apple.CoreDisplay.master, so every call is guarded (see the probe in
// ../direct-display-probe for the reason). A fault or a missing symbol degrades to "no present"
// and the keyboard read continues — never a crash, never a stuck black screen.
// ---------------------------------------------------------------------------------------------

// RECONSTRUCTED private CoreDisplay prototypes (NOT public API). Opaque pointers; verified only
// insofar as they reach a real IOSurface or report a clean failure.
typedef struct __ChooserCoreDisplay        *chooser_cdisplay_ref;
typedef struct __ChooserCoreDisplaySurface *chooser_cdisplay_surface_ref;
typedef void*            (*fn_cgx_device)(CGDirectDisplayID);
typedef chooser_cdisplay_ref (*fn_get_display)(void *);
typedef chooser_cdisplay_surface_ref (*fn_get_next_surface)(chooser_cdisplay_ref);
typedef IOSurfaceRef     (*fn_surface_get_iosurface)(chooser_cdisplay_surface_ref);
typedef int              (*fn_present_iosurface)(chooser_cdisplay_ref, IOSurfaceRef);

static void *g_core_display = NULL;
static int g_cd_resolved = 0;
static fn_cgx_device             f_cgx;
static fn_get_display            f_get_display;
static fn_get_next_surface       f_get_next;
static fn_surface_get_iosurface  f_surface_get_io;
static fn_present_iosurface      f_present_io;

static sigjmp_buf g_present_jmp;
static volatile sig_atomic_t g_present_guarded = 0;

static void present_guard_handler(int sig, siginfo_t *si, void *ctx) {
  (void)sig; (void)si; (void)ctx;
  if (g_present_guarded) siglongjmp(g_present_jmp, 1);
  signal(sig, SIG_DFL);
  raise(sig);
}

static void present_guard_install(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = present_guard_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
}

// Resolve the private CoreDisplay symbols once; 0 on success (all three needed hops present).
static int present_resolve(void) {
  if (g_cd_resolved) return g_core_display ? 0 : -1;
  g_cd_resolved = 1;
  g_core_display = dlopen("/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay", RTLD_LAZY);
  if (!g_core_display) g_core_display = dlopen("CoreDisplay", RTLD_LAZY);
  if (!g_core_display) return -1;
  f_cgx           = (fn_cgx_device)dlsym(g_core_display, "CGXDisplayDeviceForDisplayID");
  f_get_display   = (fn_get_display)dlsym(g_core_display, "CoreDisplay_GetDisplayForCGXDisplayDevice");
  f_get_next      = (fn_get_next_surface)dlsym(g_core_display, "CoreDisplay_Display_GetNextSurface");
  f_surface_get_io= (fn_surface_get_iosurface)dlsym(g_core_display, "CoreDisplay_DisplaySurface_GetIOSurface");
  f_present_io    = (fn_present_iosurface)dlsym(g_core_display, "CoreDisplay_Display_PresentIOSurface");
  return (f_cgx && f_get_display && f_present_io) ? 0 : -1;
}

// Read-only diagnostic: symbol availability + the public display list. Never flips the screen.
static int probe_present(void) {
  int ok = present_resolve();
  printf("present: CoreDisplay dlopen -> %s\n", g_core_display ? "OK" : "FAILED");
  const char *names[] = {
    "CGXDisplayDeviceForDisplayID",
    "CoreDisplay_GetDisplayForCGXDisplayDevice",
    "CoreDisplay_Display_GetNextSurface",
    "CoreDisplay_DisplaySurface_GetIOSurface",
    "CoreDisplay_Display_PresentIOSurface",
    "CoreDisplay_Display_PresentIOSurfaceWithSrcAndDstRect",
  };
  for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
    void *s = g_core_display ? dlsym(g_core_display, names[i]) : NULL;
    printf("present:   %-52s %s\n", names[i], s ? "present" : "MISSING");
  }
  printf("present: all three present hops resolved -> %s\n", ok == 0 ? "yes" : "no");

  CGDirectDisplayID displays[16];
  uint32_t n = 0;
  CGError e = CGGetActiveDisplayList(16, displays, &n);
  printf("present: CGGetActiveDisplayList -> %d display(s) (err %d)\n", n, (int)e);
  for (uint32_t i = 0; i < n && i < 16; i++) {
    size_t ww = CGDisplayPixelsWide(displays[i]), hh = CGDisplayPixelsHigh(displays[i]);
    printf("present:   display[%u] id=0x%08x %zux%zu online=%d main=%d\n",
           i, displays[i], ww, hh, CGDisplayIsOnline(displays[i]),
           CGDisplayIsMain(displays[i]));
  }
  return 0;
}

// Best-effort: map a public CGDirectDisplayID -> opaque CoreDisplayRef (guarded).
static chooser_cdisplay_ref display_ref_for_id(CGDirectDisplayID did) {
  void *dev = NULL;
  chooser_cdisplay_ref ref = NULL;
  if (sigsetjmp(g_present_jmp, 1) == 0) {
    g_present_guarded = 1;
    dev = f_cgx(did);
    if (dev) ref = f_get_display(dev);
    g_present_guarded = 0;
  } else {
    g_present_guarded = 0;
    return NULL;
  }
  return ref;
}

// Present an RGBA8 framebuffer (w x h) to the main display. Returns 0 on success, negative on
// any failure. Never faults the process. Called only when the present gate is on.
static int present_framebuffer(const uint8_t *fb, int w, int h) {
  if (present_resolve() != 0) {
    fprintf(stderr, "chooser: present: CoreDisplay present symbols unavailable\n");
    return -1;
  }
  CGDirectDisplayID did = CGMainDisplayID();
  chooser_cdisplay_ref disp = display_ref_for_id(did);
  if (!disp) {
    fprintf(stderr, "chooser: present: could not map display id 0x%08x -> CoreDisplayRef\n", did);
    return -1;
  }

  // Preferred: the display's own "next" surface (already the right size/format for scanout).
  IOSurfaceRef io = NULL;
  if (f_get_next && f_surface_get_io) {
    chooser_cdisplay_surface_ref next = NULL;
    if (sigsetjmp(g_present_jmp, 1) == 0) {
      g_present_guarded = 1;
      next = f_get_next(disp);
      if (next) io = f_surface_get_io(next);
      g_present_guarded = 0;
    } else {
      g_present_guarded = 0;
    }
  }

  // Fallback: a fresh BGRA8 surface at our framebuffer size.
  int created = 0;
  if (!io) {
    int w32 = w, h32 = h, bpr32 = w * 4, bpe32 = 4;
    uint32_t bgra = 0x42475241;  // 'BGRA'
    CFNumberRef nw = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &w32);
    CFNumberRef nh = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &h32);
    CFNumberRef nbpr = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bpr32);
    CFNumberRef nbpe = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bpe32);
    CFNumberRef npf = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bgra);
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (nw && nh && nbpr && nbpe && npf && props) {
      CFDictionarySetValue(props, kIOSurfaceWidth, nw);
      CFDictionarySetValue(props, kIOSurfaceHeight, nh);
      CFDictionarySetValue(props, kIOSurfaceBytesPerRow, nbpr);
      CFDictionarySetValue(props, kIOSurfaceBytesPerElement, nbpe);
      CFDictionarySetValue(props, kIOSurfacePixelFormat, npf);
      io = IOSurfaceCreate(props);
      created = 1;
    }
    if (nw) CFRelease(nw); if (nh) CFRelease(nh); if (nbpr) CFRelease(nbpr);
    if (nbpe) CFRelease(nbpe); if (npf) CFRelease(npf);
    if (props) CFRelease(props);
  }
  if (!io) {
    fprintf(stderr, "chooser: present: no IOSurface to write\n");
    return -1;
  }

  size_t iw = IOSurfaceGetWidth(io), ih = IOSurfaceGetHeight(io), bpr = IOSurfaceGetBytesPerRow(io);
  if (IOSurfaceLock(io, 0, NULL) == kIOReturnSuccess) {
    uint8_t *base = (uint8_t *)IOSurfaceGetBaseAddress(io);
    if (base) {
      for (size_t y = 0; y < ih && y < (size_t)h; y++) {
        const uint8_t *s = fb + y * (size_t)w * 4;
        uint8_t *d = base + y * bpr;
        for (size_t x = 0; x < iw && x < (size_t)w; x++) {
          uint8_t r = s[x*4+0], g = s[x*4+1], b = s[x*4+2], a = s[x*4+3];
          d[x*4+0] = b; d[x*4+1] = g; d[x*4+2] = r; d[x*4+3] = a;  // RGBA -> BGRA
        }
      }
    }
    IOSurfaceUnlock(io, 0, NULL);
  }

  int rc = -1;
  if (sigsetjmp(g_present_jmp, 1) == 0) {
    g_present_guarded = 1;
    rc = f_present_io(disp, io);
    g_present_guarded = 0;
  } else {
    g_present_guarded = 0;
  }
  if (created) CFRelease(io);
  fprintf(stderr, "chooser: present: %s %zux%zu surface to display 0x%08x -> rc=%d\n",
          created ? "fresh" : "display-next", iw, ih, did, rc);
  return rc;
}

// ---------------------------------------------------------------------------------------------
// Keyboard: HID stack (IOHIDManager) primary, /dev/console (fd 0) fallback.
// ---------------------------------------------------------------------------------------------

#define KEY_QUEUE_CAP 64
static int g_key_queue[KEY_QUEUE_CAP];
static int g_key_head = 0, g_key_tail = 0, g_key_count = 0;

static IOHIDManagerRef g_hid = NULL;
static int g_hid_ready = 0;

static void key_queue_push(int k) {
  if (g_key_count >= KEY_QUEUE_CAP) return;
  g_key_queue[g_key_tail] = k;
  g_key_tail = (g_key_tail + 1) % KEY_QUEUE_CAP;
  g_key_count++;
}

static int key_queue_pop(void) {
  if (g_key_count == 0) return -1;
  int k = g_key_queue[g_key_head];
  g_key_head = (g_key_head + 1) % KEY_QUEUE_CAP;
  g_key_count--;
  return k;
}

// Map a HID keyboard/keypad usage to the byte the branch logic already understands.
static int hid_usage_to_key(uint32_t usage) {
  switch (usage) {
    case kHIDUsage_Keyboard1: case kHIDUsage_Keypad1: return '1';
    case kHIDUsage_Keyboard2: case kHIDUsage_Keypad2: return '2';
    case kHIDUsage_Keyboard0: case kHIDUsage_Keypad0: return '0';
    case kHIDUsage_KeyboardM: return 'm';
    case kHIDUsage_KeyboardL: return 'l';
    case kHIDUsage_KeyboardS: return 's';
    case kHIDUsage_KeyboardReturnOrEnter: case kHIDUsage_KeypadEnter: return '\r';
    default: return 0;
  }
}

static void hid_value_cb(void *ctx, IOReturn result, void *sender, IOHIDValueRef value) {
  (void)ctx; (void)sender;
  if (result != kIOReturnSuccess) return;
  IOHIDElementRef el = IOHIDValueGetElement(value);
  if (!el) return;
  if (IOHIDValueGetIntegerValue(value) != 1) return;                 // key DOWN only
  if (IOHIDElementGetUsagePage(el) != kHIDPage_KeyboardOrKeypad) return;
  int k = hid_usage_to_key((uint32_t)IOHIDElementGetUsage(el));
  if (k) key_queue_push(k);
}

static void hid_init(void) {
  if (g_hid) return;
  g_hid = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
  if (!g_hid) { fprintf(stderr, "chooser: hid: IOHIDManagerCreate failed\n"); return; }

  int page = kHIDPage_GenericDesktop;
  int usage = kHIDUsage_GD_Keyboard;
  CFNumberRef pn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page);
  CFNumberRef un = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
  CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (pn && un && match) {
    CFDictionarySetValue(match, CFSTR(kIOHIDDeviceUsagePageKey), pn);
    CFDictionarySetValue(match, CFSTR(kIOHIDDeviceUsageKey), un);
    IOHIDManagerSetDeviceMatching(g_hid, match);
  }
  if (pn) CFRelease(pn);
  if (un) CFRelease(un);
  if (match) CFRelease(match);

  IOHIDManagerRegisterInputValueCallback(g_hid, hid_value_cb, NULL);
  IOHIDManagerScheduleWithRunLoop(g_hid, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
  IOReturn r = IOHIDManagerOpen(g_hid, kIOHIDOptionsTypeNone);
  if (r == kIOReturnSuccess) {
    g_hid_ready = 1;
    CFSetRef devs = IOHIDManagerCopyDevices(g_hid);
    CFIndex n = devs ? CFSetGetCount(devs) : 0;
    fprintf(stderr, "chooser: hid: IOHIDManager open, %ld keyboard device(s) matched\n", (long)n);
    if (devs) CFRelease(devs);
  } else {
    g_hid_ready = 0;
    fprintf(stderr, "chooser: hid: IOHIDManagerOpen failed (0x%x); /dev/console fallback active\n",
            (unsigned)r);
  }
}

// Read-only diagnostic: open the HID manager and list the keyboards it matched. Never reads keys.
static int probe_hid(void) {
  hid_init();
  printf("hid: IOHIDManager %s\n", g_hid_ready ? "open" : "unavailable");
  if (g_hid) {
    CFSetRef devs = IOHIDManagerCopyDevices(g_hid);
    if (devs) {
      CFIndex n = CFSetGetCount(devs);
      printf("hid: %ld keyboard device(s)\n", (long)n);
      CFRelease(devs);
    } else {
      printf("hid: IOHIDManagerCopyDevices -> NULL\n");
    }
  }
  printf("hid: fallback console fd 0 is %s\n", isatty(0) ? "a tty" : "not a tty");
  return 0;
}

// One ~20ms key-pump slice: drain the HID queue, run the run loop briefly so HID callbacks fire,
// then check /dev/console (fd 0) non-blocking. Returns a key byte or -1.
static int get_key(void) {
  int k = key_queue_pop();
  if (k >= 0) return k;
  if (g_hid_ready) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
  k = key_queue_pop();
  if (k >= 0) return k;
  struct pollfd pfd;
  pfd.fd = 0; pfd.events = POLLIN; pfd.revents = 0;
  if (poll(&pfd, 1, 0) <= 0) return -1;
  unsigned char c = 0;
  if (read(0, &c, 1) != 1) return -1;
  return (int)c;
}

// ---------------------------------------------------------------------------------------------
// The menu loop: render + (optional) present, then read keys / count down.
// ---------------------------------------------------------------------------------------------

static void menu_fb_size(int present_enabled, int *w, int *h) {
  *w = MENU_DEFAULT_W; *h = MENU_DEFAULT_H;
  if (!present_enabled) return;
  CGDirectDisplayID displays[16];
  uint32_t n = 0;
  if (CGGetActiveDisplayList(16, displays, &n) != kCGErrorSuccess || n == 0) return;
  CGDirectDisplayID main = CGMainDisplayID();
  for (uint32_t i = 0; i < n; i++) {
    if (displays[i] == main) { main = displays[i]; break; }
  }
  size_t ww = CGDisplayPixelsWide(main), hh = CGDisplayPixelsHigh(main);
  if (ww > 0 && hh > 0 && ww <= 16384 && hh <= 16384) { *w = (int)ww; *h = (int)hh; }
}

static branch_t run_menu(int timeout_s, int present_enabled) {
  hid_init();   // idempotent; no-op after the first call. HID primary, /dev/console fallback.
  int w, h;
  menu_fb_size(present_enabled, &w, &h);
  int scale = menu_scale(w);
  uint8_t *fb = (uint8_t *)calloc(1, (size_t)w * (size_t)h * 4);
  if (!fb) {
    fprintf(stderr, "chooser: menu framebuffer alloc failed (%dx%d); macOS default\n", w, h);
    return BRANCH_MACOS;
  }

  int remaining = timeout_s;
  render_menu(fb, w, h, scale, remaining);
  if (present_enabled) {
    int pr = present_framebuffer(fb, w, h);
    if (pr != 0)
      fprintf(stderr, "chooser: menu present failed (rc=%d); keyboard still live\n", pr);
  }

  int64_t deadline = now_ms() + (int64_t)timeout_s * 1000;
  int last_remaining = remaining;
  for (;;) {
    int key = get_key();
    if (key >= 0) {
      branch_t b = key_to_branch(key);
      printf("chooser: key '%c' -> %s\n", (char)key, branch_name(b));
      fflush(stdout);
      free(fb);
      return b;
    }
    int64_t now = now_ms();
    if (now >= deadline) break;
    int nr = (int)((deadline - now + 999) / 1000);
    if (nr != last_remaining) {
      last_remaining = nr;
      render_menu(fb, w, h, scale, nr);
      if (present_enabled) present_framebuffer(fb, w, h);
    }
  }

  printf("chooser: timeout / no input -> macOS\n");
  free(fb);
  return BRANCH_MACOS;
}

// Render the menu to a PPM (a CAPTURED framebuffer — never the live display) and exit.
static int render_mode(const char *path, int w, int h) {
  int t = boot_timeout();
  int scale = menu_scale(w);
  uint8_t *fb = (uint8_t *)calloc(1, (size_t)w * (size_t)h * 4);
  if (!fb) { fprintf(stderr, "chooser: render alloc failed\n"); return 1; }
  render_menu(fb, w, h, scale, t);
  if (write_ppm(path, fb, w, h) != 0) { perror("chooser: render write"); free(fb); return 1; }
  printf("render: wrote %s (%dx%d, scale %d, countdown %d s, %zu lit pixels)\n",
         path, w, h, scale, t, count_nonblank(fb, w, h));
  printf("render: menu text:\n"
         "  shimmy\n  [1] Boot macOS\n  [2] Boot shimmy (Linux)\n  (defaults to macOS in %d s)\n",
         t);
  free(fb);
  return 0;
}

static branch_t resolve_branch(const char *forced, int present_enabled) {
  if (forced) return forced_branch(forced);
  const char *env = getenv("SHIMMY_BOOT_CHOICE");
  if (env && *env) return forced_branch(env);
  return run_menu(boot_timeout(), present_enabled);
}

// Test hook: check in ONE arbitrary name and hold it. Used with a scratch launchd job in the
// caller's own gui domain to prove the check-in path headlessly (see the validation report).
static int do_checkin(const char *name, int hold_s) {
  mach_port_t bp = MACH_PORT_NULL;
  kern_return_t kr = task_get_bootstrap_port(mach_task_self(), &bp);
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "checkin: task_get_bootstrap_port failed: %s\n", mach_error_string(kr));
    return 1;
  }
  mach_port_t port = MACH_PORT_NULL;
  kr = bootstrap_check_in(bp, name, &port);
  printf("checkin: bootstrap_check_in(%s) -> kr=%d (%s) port=0x%x\n",
         name, kr, bootstrap_strerror(kr), port);
  fflush(stdout);
  if (kr != BOOTSTRAP_SUCCESS) return 1;
  printf("checkin: holding %s for %d s (verify with: launchctl print ...)\n", name, hold_s);
  fflush(stdout);
  sleep((unsigned)hold_s);
  return 0;
}

// "1" means the flag is on; any other value (or unset) means off.
static int env_flag(const char *name) {
  const char *v = getenv(name);
  return v && strcmp(v, "1") == 0;
}

static const char *windowserver_cmd(void) {
  const char *v = getenv("SHIMMY_WINDOWSERVER_CMD");
  return (v && *v) ? v : SHIMMY_WINDOWSERVER_DEFAULT_PATH;
}

// Default: system mode (real check-in). SHIMMY_WINDOWSERVER_SYSTEM=0 selects the stand-in's
// safe test-name mode for headless validation.
static int windowserver_system_mode(void) {
  const char *v = getenv("SHIMMY_WINDOWSERVER_SYSTEM");
  return !(v && strcmp(v, "0") == 0);
}

static int warm_switch_kickstart_enabled(void) {
  return env_flag("SHIMMY_WARM_SWITCH_KICKSTART");
}

static int present_enabled_flag(int explicit_flag) {
  return explicit_flag || env_flag("SHIMMY_PRESENT");
}

static int branch_macos(int kickstart, int dry_run) {
  printf("chooser: branch = macOS (normal boot)\n");
  if (kickstart) {
    if (dry_run) {
      printf("chooser: dry-run: would kickstart system/com.apple.WindowServer\n");
    } else {
      printf("chooser: kickstarting system/com.apple.WindowServer\n");
      int rc = system("/bin/launchctl kickstart -k system/com.apple.WindowServer");
      if (rc != 0) {
        fprintf(stderr, "chooser: kickstart returned %d (ignored)\n", rc);
      }
    }
  }
  // Normal path: exit 0; loginwindow/launchd start the real WindowServer on demand.
  return 0;
}

// Reap `pid` after a SIGTERM, escalating to SIGKILL if it does not exit within 5 s. The
// stand-in installs a SIGTERM handler that exits cleanly; this is the fail-safe watchdog.
static void wait_with_escalation(pid_t pid, int *status) {
  int waited_ms = 0;
  for (;;) {
    pid_t r = waitpid(pid, status, WNOHANG);
    if (r == pid) return;
    if (r < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (waited_ms >= 5000) {
      fprintf(stderr, "chooser: stand-in did not exit after SIGTERM; sending SIGKILL\n");
      kill(pid, SIGKILL);
      waitpid(pid, status, 0);
      return;
    }
    usleep(100 * 1000);
    waited_ms += 100;
  }
}

static int branch_shimmy(int dry_run) {
  const char *ws = windowserver_cmd();
  int system_mode = windowserver_system_mode();
  int warm_kick = warm_switch_kickstart_enabled();

  printf("chooser: branch = shimmy (takeover)\n");
  printf("chooser: stand-in = %s (%s)\n", ws,
         system_mode ? "RDNATIVE_WINDOWSERVER_SYSTEM=1 -> real check-in"
                     : "test-name mode (safe: no real check-in)");

  if (dry_run) {
    printf("chooser: dry-run: would fork+exec %s with RDNATIVE_WINDOWSERVER_SYSTEM=1 so it\n",
           ws);
    printf("chooser: dry-run:   checks in %s then %s and runs the ~37-function server,\n",
           SVC_MAIN, SVC_ACTIVE);
    printf("chooser: dry-run:   then on clean exit / macOS key (1/m) warm-switch to the real "
           "WindowServer\n");
    printf("chooser: dry-run:   warm-switch kickstart: %s\n",
           warm_kick ? "launchctl kickstart -k system/com.apple.WindowServer"
                     : "disabled (print only)");
    return 0;
  }

  if (access(ws, X_OK) != 0) {
    fprintf(stderr, "chooser: stand-in not executable: %s (%s); fail-safe -> macOS\n",
            ws, strerror(errno));
    return branch_macos(warm_kick, 0);
  }

  pid_t child = fork();
  if (child < 0) {
    perror("chooser: fork");
    return branch_macos(warm_kick, 0);
  }
  if (child == 0) {
    // Child: become the WindowServer stand-in. It performs the real bootstrap check-in and
    // runs the server loop. (Test mode omits the env var, so it registers test names.)
    if (system_mode) setenv("RDNATIVE_WINDOWSERVER_SYSTEM", "1", 1);
    execl(ws, ws, (char *)NULL);
    perror("chooser: execl stand-in");
    _exit(127);
  }

  // Parent: stay alive as the launchd job's primary process; wait for a clean stand-in exit
  // OR a "macOS" key (which asks the stand-in to shut down cleanly).
  printf("chooser: stand-in pid %d running (macOS key 1/m or SIGTERM to warm-switch)\n",
         (int)child);
  fflush(stdout);

  int status = 0;
  int reaped = 0;
  for (;;) {
    pid_t r = waitpid(child, &status, WNOHANG);
    if (r == child) { reaped = 1; break; }
    if (r < 0) {
      if (errno == EINTR) continue;
      perror("chooser: waitpid");
      break;
    }
    int key = poll_key(250);   // 250 ms cadence so we reap promptly
    if (key >= 0 && is_warm_switch_key(key)) {
      printf("chooser: macOS key '%c' -> warm-switch (SIGTERM to stand-in %d)\n",
             key, (int)child);
      fflush(stdout);
      if (kill(child, SIGTERM) != 0 && errno != ESRCH) perror("chooser: kill(SIGTERM)");
      wait_with_escalation(child, &status);
      reaped = 1;
      break;
    }
  }
  if (!reaped) {
    // waitpid errored out (non-EINTR); block-reap to avoid leaving a zombie.
    waitpid(child, &status, 0);
  }

  int clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (WIFEXITED(status)) {
    printf("chooser: stand-in exited (status %d)\n", WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    printf("chooser: stand-in terminated by signal %d\n", WTERMSIG(status));
  }
  if (!clean) {
    fprintf(stderr, "chooser: stand-in did not exit cleanly; fail-safe -> macOS\n");
  }

  // The stand-in's exit already released the two receive rights it checked in, so the service
  // names are dead. Warm-switch: relaunch the real WindowServer.
  printf("chooser: warm-switch: handing back to the real WindowServer\n");
  return branch_macos(warm_kick, 0);
}

int main(int argc, char **argv) {
  int dry_run = 0;
  int kickstart = 0;
  int present_flag = 0;
  int probe_present_flag = 0;
  int probe_hid_flag = 0;
  const char *forced = NULL;
  const char *checkin_name = NULL;
  int checkin_hold = 30;
  const char *render_path = NULL;
  int render_w = MENU_DEFAULT_W, render_h = MENU_DEFAULT_H;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--dry-run") || !strcmp(a, "-n")) {
      dry_run = 1;
    } else if (!strcmp(a, "--choice") && i + 1 < argc) {
      forced = argv[++i];
    } else if (!strcmp(a, "--kickstart-windowserver")) {
      kickstart = 1;
    } else if (!strcmp(a, "--present")) {
      present_flag = 1;
    } else if (!strcmp(a, "--probe-present")) {
      probe_present_flag = 1;
    } else if (!strcmp(a, "--probe-hid")) {
      probe_hid_flag = 1;
    } else if (!strcmp(a, "--render") && i + 1 < argc) {
      render_path = argv[++i];
    } else if (!strcmp(a, "--render-size") && i + 1 < argc) {
      int w = 0, h = 0;
      if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 16384 && h <= 16384) {
        render_w = w; render_h = h;
      } else {
        fprintf(stderr, "chooser: bad --render-size (want WxH): %s\n", a);
        return 2;
      }
    } else if (!strcmp(a, "--checkin") && i + 1 < argc) {
      checkin_name = argv[++i];
      if (i + 1 < argc && argv[i + 1][0] != '-') checkin_hold = atoi(argv[++i]);
    } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
      usage(stdout);
      return 0;
    } else {
      fprintf(stderr, "chooser: unknown argument: %s\n", a);
      usage(stderr);
      return 2;
    }
  }

  setbuf(stdout, NULL);
  present_guard_install();

  // Read-only diagnostic modes never touch the screen or branch.
  if (checkin_name) {
    return do_checkin(checkin_name, checkin_hold);
  }
  if (probe_present_flag) {
    return probe_present();
  }
  if (probe_hid_flag) {
    return probe_hid();
  }
  if (render_path) {
    return render_mode(render_path, render_w, render_h);
  }

  int present_enabled = present_enabled_flag(present_flag);

  branch_t b = resolve_branch(forced, present_enabled);
  if (dry_run) {
    printf("chooser: dry-run -> %s\n", branch_name(b));
  }
  if (b == BRANCH_SHIMMY) return branch_shimmy(dry_run);
  return branch_macos(kickstart, dry_run);
}
