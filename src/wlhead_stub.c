// wlhead_stub.c — headless stand-ins for the Metal presentation + host input entry points that
// wlsrv.c calls. Lets the Wayland compositor be linked into a CLI (no Cocoa/Metal) build so the
// protocol path can be traced without taking the window-server lock.
//
// INPUT INJECTION. With no Cocoa there is no NSEvent source, so the two host-input entry points
// are driven from the environment instead. This is what makes "does a click reach the client,
// and which wl_pointer object does it reach" answerable without a window:
//
//   RDWLH_CURSOR="x,y;x,y;..."   gpuwin_cursor() walks this path, one step per RDWLH_CURSOR_MS
//                                (default 16ms, one compositor tick), wrapping.
//                                Feeds pump_pointer_poll() -> wl_pointer.motion.
//   RDWLH_CURSOR_MS=n            milliseconds per path step. 0 is not accepted.
//   RDWLH_CLICK=n                after n gpuwin_next_event() calls, emit BTN_LEFT down then up
//                                at the current cursor point. 0 (default) disables.
//   RDWLH_CLICKS="n@x,y;n@x,y"   the same thing, but MANY clicks at STATED points, each fired
//                                after its own call count. Supersedes RDWLH_CLICK when set.
//
// Why RDWLH_CLICKS exists: one click cannot test a menu. Opening a menu and then choosing an item
// is two clicks at two places, and until now that was only possible in the on-screen build via
// RDINPUT_SELFTEST_MOUSE — which needs the window-server lock, and therefore serialises every
// agent behind one screen. A menu is also precisely where the interesting bugs are (popup
// placement, surface-local hit-testing, popup_done), so the headless build could trace the one
// path it most needed to trace only by borrowing the lock. It no longer has to.
//
// Each step names its own coordinate rather than relying on RDWLH_CURSOR's wrapping walk, because
// a click whose position depends on how many times an unrelated poll happened to run is not
// reproducible, and reproducibility is the entire point of the headless build.
//
// CONCURRENCY, learned the hard way: the AF_UNIX namespace is a FIXED path, so two shim instances
// unlink each other's compositor socket and the loser dies with "The Wayland connection broke".
// Set RDUNIXPREFIX to something unique per instance. Results gathered without it, while another
// instance was running, are worthless — and they do not look worthless, they look like an
// application bug in whatever you were testing.
//
// Coordinates here are already window-local, top-left origin — exactly what window.m's
// gpuwin_cursor() produces — so this injects BELOW the Cocoa transform on purpose. The Cocoa
// transform itself is covered separately, and deterministically, by src/test_pointer.m.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "gpu.h"

void x11rnd_open(uint32_t w, uint32_t h) {
  fprintf(stderr, "[wlh] x11rnd_open %ux%u (headless: no window)\n", w, h);
}
void x11rnd_present(const uint8_t* rgba, uint32_t w, uint32_t h) {
  static unsigned n;
  (void)rgba;
  if (++n <= 3 || n % 60 == 0) fprintf(stderr, "[wlh] x11rnd_present #%u %ux%u\n", n, w, h);
}
void x11rnd_present_bgra(const uint8_t* bgra, uint32_t w, uint32_t h) {
  x11rnd_present(bgra, w, h);            // headless: byte order is nobody's business here
}
int  x11rnd_poll(void) { return 0; }

// ---------------------------------------------------------------------------------------------
// The CALayer/IOSurface path does not exist without a window server, so the headless build says
// so — LOUDLY, once, if it was asked for. A silent "off" here is exactly how a measurement gets
// attributed to a path that never ran: WLSRV_CALAYER=1 on shim_wlh would otherwise produce a
// perfectly ordinary run of the Metal-path code and a set of numbers labelled "CA".

// THE WALK IS ON A CLOCK, NOT ON A CALL COUNT. It used to advance one step per read, which made
// gpuwin_cursor() a getter with a side effect: every subsystem that reads the cursor moved it.
// In the on-screen build there are three such readers (wlsrv's pump_pointer_poll, x11srv's
// QueryPointer — which is CLIENT-driven, so any xcb app advances it — and the GPU_WIN_CURSOR
// hypercall), so the path was traversed at a rate set by unrelated code. Worse, the aliasing runs
// BOTH ways: measured on the old code, a 2-point path read twice per tick returns the SAME point
// every tick, i.e. the harness silently reports a pinned cursor while configured to jiggle, and a
// 3-point path read twice per tick is traversed OUT OF ORDER (1st, 3rd, 2nd). A control that goes
// still when you add a reader is not a control. Same reasoning as RDWLH_CLICKS' own note above:
// anything whose position depends on how many times an unrelated poll ran is not reproducible.
static int g_pts[64][2], g_npts;
static int g_cur_x, g_cur_y;
static uint64_t g_t0, g_step_ns = 16ull * 1000000ull;   // 16ms == one compositor poll tick

static pthread_once_t g_inject_once = PTHREAD_ONCE_INIT;

// Current point on the path. PURE: reading it does not move it.
static void fake_point(int* x, int* y) {
  uint64_t dt = clock_gettime_nsec_np(CLOCK_MONOTONIC) - g_t0;
  uint64_t i = (dt / g_step_ns) % (uint64_t)g_npts;
  *x = g_pts[i][0]; *y = g_pts[i][1];
}

static void inject_init_once(void) {
  const char* e = getenv("RDWLH_CURSOR");
  for (const char* p = e; p && *p && g_npts < 64; ) {
    const char* comma = strchr(p, ',');
    if (!comma) break;
    g_pts[g_npts][0] = atoi(p);
    g_pts[g_npts][1] = atoi(comma + 1);
    g_npts++;
    const char* semi = strchr(comma, ';');
    if (!semi) break;
    p = semi + 1;
  }
  const char* ms = getenv("RDWLH_CURSOR_MS");
  if (ms && *ms) { double d = atof(ms); if (d > 0) g_step_ns = (uint64_t)(d * 1e6); }
  if (g_npts) {
    g_t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    g_cur_x = g_pts[0][0]; g_cur_y = g_pts[0][1];       // valid before the first read
    fprintf(stderr, "[wlh] cursor path: %d point(s), one step per %.0f ms\n",
            g_npts, (double)g_step_ns / 1e6);
  }
}

static void inject_init(void) { pthread_once(&g_inject_once, inject_init_once); }

// Window identity is a no-op headless: there is no NSWindow to name. Kept as real symbols so
// wlsrv can call them unconditionally rather than growing #ifdefs at every call site.
void gpuwin_set_title(const char* utf8)   { (void)utf8; }
void gpuwin_set_appname(const char* utf8) { (void)utf8; }
void gpuwin_minimize(void)                { }
void gpuwin_clipboard_write(const char* mime, const char* data, size_t len)
  { (void)mime; (void)data; (void)len; }
int gpuwin_clipboard_read(const char* mime, char* out, size_t cap)
  { (void)mime; (void)out; (void)cap; return -1; }
int gpuwin_clipboard_count(void) { return 0; }

void gpuwin_cursor(int* x, int* y) {
  inject_init();
  if (g_npts) fake_point(&g_cur_x, &g_cur_y);
  if (x) *x = g_cur_x;
  if (y) *y = g_cur_y;
}

// RDWLH_CLICKS="n@x,y;n@x,y;..." — parsed once. Steps fire in the order given, each when the
// drain-call counter reaches its n; an out-of-order n simply fires as soon as the counter passes
// it, which keeps a mis-sorted script debuggable rather than silently dead.
#define WLH_MAX_CLICKS 32
static struct { long at; int x, y; } g_clicks[WLH_MAX_CLICKS];
static int g_nclicks, g_click_i, g_clicks_inited;

static void clicks_init(void) {
  if (g_clicks_inited) return;
  g_clicks_inited = 1;
  const char* e = getenv("RDWLH_CLICKS");
  if (!e || !*e) return;
  for (const char* p = e; *p && g_nclicks < WLH_MAX_CLICKS; ) {
    char* end = NULL;
    long at = strtol(p, &end, 10);
    if (end == p || *end != '@') break;
    p = end + 1;
    int x = (int)strtol(p, &end, 10);
    if (end == p || *end != ',') break;
    p = end + 1;
    int y = (int)strtol(p, &end, 10);
    if (end == p) break;
    g_clicks[g_nclicks].at = at;
    g_clicks[g_nclicks].x  = x;
    g_clicks[g_nclicks].y  = y;
    g_nclicks++;
    p = end;
    while (*p == ';') p++;
  }
  if (g_nclicks) {
    fprintf(stderr, "[wlh] click script: %d step(s):", g_nclicks);
    for (int i = 0; i < g_nclicks; i++)
      fprintf(stderr, " %ld@(%d,%d)", g_clicks[i].at, g_clicks[i].x, g_clicks[i].y);
    fprintf(stderr, "\n");
  }
}

int gpuwin_next_event(struct gpuwin_event* out) {
  inject_init();
  clicks_init();
  // The poll path is gated on a pointer existing, so gpuwin_cursor() may never have run before a
  // click is injected. inject_init_once() seeds g_cur_x/g_cur_y from the first path point for
  // exactly that case, so an injected click always carries a real coordinate.
  static long calls;
  static int fired, pending_up;
  calls++;
  if (!out) return 0;

  if (pending_up) {                                    // release, one drain later
    pending_up = 0;
    memset(out, 0, sizeof *out);
    out->type = GPUWIN_EV_BUTTON; out->code = 0x110; out->state = 0;
    out->x = g_cur_x; out->y = g_cur_y;
    fprintf(stderr, "[wlh] INJECT button up   at (%d,%d)\n", out->x, out->y);
    return 1;
  }

  if (g_nclicks) {                                     // multi-click script wins when present
    if (g_click_i >= g_nclicks) return 0;
    if (calls < g_clicks[g_click_i].at) return 0;
    // The click's own coordinate also becomes the cursor position, so the release that follows
    // lands at the same place and any subsequent poll reports where the click actually went.
    g_cur_x = g_clicks[g_click_i].x;
    g_cur_y = g_clicks[g_click_i].y;
    g_click_i++;
    pending_up = 1;
    memset(out, 0, sizeof *out);
    out->type = GPUWIN_EV_BUTTON; out->code = 0x110; out->state = 1;
    out->x = g_cur_x; out->y = g_cur_y;
    fprintf(stderr, "[wlh] INJECT button down at (%d,%d)  [step %d/%d]\n",
            out->x, out->y, g_click_i, g_nclicks);
    return 1;
  }

  const char* c = getenv("RDWLH_CLICK");
  long at = c ? atol(c) : 0;
  if (at <= 0) return 0;
  if (!fired && calls >= at) {
    fired = 1; pending_up = 1;
    memset(out, 0, sizeof *out);
    out->type = GPUWIN_EV_BUTTON; out->code = 0x110; out->state = 1;
    out->x = g_cur_x; out->y = g_cur_y;
    fprintf(stderr, "[wlh] INJECT button down at (%d,%d)\n", out->x, out->y);
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------------------------
// S3's PROBE, and it is testing ONE CLAIM: that "the CA arm can never be gated headlessly" is a
// BUILD CHOICE, not a property of CALayer/IOSurface.
//
// Both S1 and I originally recorded the gate loss as a hard cost. S1's own test_capresent then
// contradicted it -- it builds a CALayer and publishes IOSurfaces with no window and no GUI lock.
// The only thing stopping shim_wlh from running the real pixel path is that nothing calls
// capres_make_layer() here, because window.m is the only caller and there is no window.
//
// So: call it. If the discriminator prints and the DUMPED PPMs still satisfy c1px.py's closed
// form, then c1-gate covers the CA path headlessly and the cost is recoverable.
void* capres_make_layer(void);
__attribute__((constructor)) static void s3_force_ca_layer(void) {
  const char* e = getenv("WLSRV_CALAYER");
  if (e && *e && *e != '0') {
    void* l = capres_make_layer();
    fprintf(stderr, "[s3] forced capres_make_layer() in the headless build -> %s\n",
            l ? "layer created" : "NULL (mode off)");
  }
}
