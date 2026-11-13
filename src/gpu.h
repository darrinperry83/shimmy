// gpu.h — Metal bring-up, the Cocoa main-loop owner, and the guest-visible window.
//
// THREADING CONTRACT: Cocoa/Metal own the MAIN thread. gpu_run_event_loop() is called on main
// and never returns. Every gpuwin_* entry point is safe to call from the guest/X-server threads;
// they marshal to main internally.
#ifndef RDRIFT_GPU_H
#define RDRIFT_GPU_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// Guest->host GPU hypercall payload (guest pointers are real host addresses; the guest shares
// our address space). op selects the family: 0x1000 compute, 0x2000 window, 0x3000 GL.
struct gpu_req {
  uint32_t op;
  uint32_t flags;
  uint64_t a0, a1, a2, a3;
  uint64_t result;
};

void gpu_init(void);                       // main thread: create MTLDevice/queue. No window yet.
long gpu_hypercall(struct gpu_req* r);     // any thread; negative errno on failure
void gpu_run_event_loop(void);             // main thread only; runs [NSApp run], never returns

// --- the Metal-backed window the X11/Wayland servers present into ---
long gpuwin_open(uint64_t w, uint64_t h);                          // create+show (idempotent)
long gpuwin_present(const uint8_t* rgba, uint64_t w, uint64_t h);  // upload+blit+present a frame
// `bgra`: 0 = the framebuffer is R,G,B,A in memory (x11srv.c's), 1 = B,G,R,A (a wl_shm buffer's).
// The staging texture's pixel format follows it, so case 1 costs NOTHING — B,G,R,A is Metal's
// native MTLPixelFormatBGRA8Unorm and the sampler unpacks it for free. Stating the order per call
// is deliberate: a single global format would silently swap red and blue on the other caller.
long gpuwin_present_fmt(const uint8_t* px, uint64_t w, uint64_t h, int bgra);
int  gpuwin_poll(void);                                            // pump events; !=0 if closed
// Live cursor in window-local X11 coords (top-left origin), clamped to the content rect.
// MUST NOT depend on gpuwin_poll() being called: under [NSApp run] it never is.
void gpuwin_cursor(int* x, int* y);

// macOS window identity, driven by what the Linux client already tells the compositor.
void gpuwin_set_title(const char* utf8);      // xdg_toplevel.set_title  -> NSWindow.title
void gpuwin_set_appname(const char* utf8);    // xdg_toplevel.set_app_id -> Dock / menu-bar name
void gpuwin_minimize(void);                   // xdg_toplevel.set_minimized
void gpuwin_clipboard_write(const char* mime, const char* data, size_t len);
int  gpuwin_clipboard_read(const char* mime, char* out, size_t cap);
int  gpuwin_clipboard_count(void);

// --- input events, host -> guest -------------------------------------------------------------
// Polling the cursor was enough for xeyes, but a real client needs discrete events: key presses,
// button presses, scroll. Cocoa delivers those to the MAIN thread under -[NSApp run], while the
// display server runs on its own thread, so they are captured by a local NSEvent monitor and
// queued here for the server to drain.
//
// `code` is a LINUX EVDEV keycode for GPUWIN_EV_KEY and an evdev button code (BTN_LEFT 0x110,
// BTN_RIGHT 0x111, BTN_MIDDLE 0x112) for GPUWIN_EV_BUTTON. That is what Wayland's wl_keyboard /
// wl_pointer carry on the wire, so no further translation is needed there; an X11 server would
// add 8 to get an X keycode.
#define GPUWIN_EV_KEY     1      // code = evdev keycode,  state = 1 down / 0 up
#define GPUWIN_EV_BUTTON  2      // code = evdev button,   state = 1 down / 0 up
#define GPUWIN_EV_MOTION  3      // x, y = window-local, top-left origin
#define GPUWIN_EV_SCROLL  4      // x, y = discrete steps (positive = down / right)
#define GPUWIN_EV_ENTER   5      // pointer entered the window
#define GPUWIN_EV_LEAVE   6      // pointer left the window
#define GPUWIN_EV_FOCUS   7      // state = 1 gained / 0 lost
#define GPUWIN_EV_RESIZE  8      // x, y = requested content width, height

// Modifier bitmask, matching the Wayland/XKB depressed-modifier bit order.
#define GPUWIN_MOD_SHIFT  (1u << 0)
#define GPUWIN_MOD_CAPS   (1u << 1)
#define GPUWIN_MOD_CTRL   (1u << 2)
#define GPUWIN_MOD_ALT    (1u << 3)
#define GPUWIN_MOD_LOGO   (1u << 6)

struct gpuwin_event {
  uint32_t type;      // GPUWIN_EV_*
  uint32_t code;
  uint32_t state;
  uint32_t mods;
  int32_t  x, y;
};

// Pop the oldest queued event. Returns 1 and fills *out, or 0 when the queue is empty.
// Safe to call from any thread; does NOT require gpuwin_poll().
int  gpuwin_next_event(struct gpuwin_event* out);

#ifdef __cplusplus
}
#endif
#endif
