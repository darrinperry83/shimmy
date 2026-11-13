// test_wlpointer.c — headless wl_pointer protocol harness for the in-process compositor.
//
//   clang -arch arm64 -O1 -Isrc -o build/test_wlpointer src/test_wlpointer.c src/wlsrv.c \
//     && ./build/test_wlpointer
//
// No Metal, no window, no display: the host-window and renderer entry points are stubbed, so this
// runs anywhere and needs nothing on screen. Exit status is the verdict.
//
// Links src/wlsrv.c against stub host-window and renderer entry points, then speaks raw Wayland
// wire format at it as a client, replaying the exact object sequence GTK4 performs:
//
//   toplevel:  create_surface -> get_xdg_surface -> get_toplevel -> commit -> ack_configure
//   popup:     create_surface -> get_xdg_surface -> get_popup -> commit -> ack -> DESTROY ALL
//   then:      move the pointer and click, and see which surface id the compositor addresses.
//
// The whole point is the final step. Nothing here needs a window, a display or the GUI lock.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include "gpu.h"
#include "wlsrv.h"

// ---------------------------------------------------------------- host stubs
static struct gpuwin_event g_q[64];
static int g_qh, g_qt;
static int g_cx, g_cy;

void x11rnd_open(uint32_t w, uint32_t h) { (void)w; (void)h; }
void x11rnd_present(const uint8_t* rgba, uint32_t w, uint32_t h) { (void)rgba; (void)w; (void)h; }
void x11rnd_present_bgra(const uint8_t* p, uint32_t w, uint32_t h) { (void)p; (void)w; (void)h; }
void gpuwin_cursor(int* x, int* y) { *x = g_cx; *y = g_cy; }
int gpuwin_next_event(struct gpuwin_event* out) {
  if (g_qh == g_qt) return 0;
  *out = g_q[g_qh]; g_qh = (g_qh + 1) % 64; return 1;
}
static void push(uint32_t type, uint32_t code, uint32_t state, int x, int y) {
  struct gpuwin_event e; memset(&e, 0, sizeof e);
  e.type = type; e.code = code; e.state = state; e.x = x; e.y = y;
  g_q[g_qt] = e; g_qt = (g_qt + 1) % 64;
}

// ------------------------------------------------------------- client plumbing
static int cfd;
static uint32_t rd32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t* p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
// req(target, opcode, argc, args...) — every argument we need is a plain u32.
static void req(uint32_t id, uint16_t op, int argc, ...) {
  uint8_t b[256]; size_t n = 8;
  va_list ap; va_start(ap, argc);
  for (int i = 0; i < argc; i++) { wr32(b + n, va_arg(ap, uint32_t)); n += 4; }
  va_end(ap);
  wr32(b, id); wr32(b + 4, (uint32_t)op | ((uint32_t)n << 16));
  if (write(cfd, b, n) != (ssize_t)n) { perror("write"); exit(1); }
}

// Everything the compositor has sent us, in arrival order.
static uint8_t ev[65536];
static size_t evn;
static void drain(int ms) {
  struct pollfd p = { cfd, POLLIN, 0 };
  while (poll(&p, 1, ms) > 0 && (p.revents & POLLIN)) {
    ssize_t r = read(cfd, ev + evn, sizeof ev - evn);
    if (r <= 0) break;
    evn += (size_t)r;
    ms = 30;                                   // subsequent waits are short
  }
}

// wl_registry.bind(name, string iface, version, new_id) — the one request with a string arg.
static int bind_global(uint32_t reg, uint32_t name, const char* iface, uint32_t ver, uint32_t nid) {
  uint8_t b[128]; size_t n = 8;
  wr32(b + n, name); n += 4;
  size_t l = strlen(iface) + 1;
  wr32(b + n, (uint32_t)l); n += 4;
  memcpy(b + n, iface, l); n += l;
  while (n & 3) b[n++] = 0;
  wr32(b + n, ver); n += 4;
  wr32(b + n, nid); n += 4;
  wr32(b, reg); wr32(b + 4, 0u | ((uint32_t)n << 16));
  if (write(cfd, b, n) != (ssize_t)n) { perror("bind"); return -1; }
  return 0;
}

// wl_pointer opcodes we care about.
static const char* pop(uint16_t op) {
  switch (op) {
    case 0: return "enter"; case 1: return "leave"; case 2: return "motion";
    case 3: return "button"; case 4: return "axis"; case 5: return "FRAME";
    case 6: return "axis_source"; case 7: return "axis_stop"; case 8: return "axis_discrete";
    default: return "?";
  }
}

int main(int argc, char** argv) {
  const char* sock = argc > 1 ? argv[1] : "/tmp/g3-wl-harness.sock";
  unlink(sock);
  if (wlsrv_start(sock) != 0) { fprintf(stderr, "wlsrv_start failed\n"); return 1; }
  usleep(100000);

  cfd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX; snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
  if (connect(cfd, (struct sockaddr*)&sa, sizeof sa) < 0) { perror("connect"); return 1; }

  // --- bind the globals GTK4 binds -----------------------------------------
  enum { REG = 2, COMP = 3, SEAT = 4, WMB = 5, PTR = 6,
         TOP_SURF = 10, TOP_XDG = 11, TOP_TL = 12,
         POS = 20, POP_SURF = 21, POP_XDG = 22, POP_POPUP = 23,
         SEAT2 = 30, PTR2 = 31 };
  req(1, 1, 1, (uint32_t)REG);                                   // wl_display.get_registry
  drain(150);

  // wl_registry.bind is (name, string iface, version, new_id) — hand-rolled for the string.
  { struct { const char* n; uint32_t name, ver, nid; } binds[] = {
      { "wl_compositor", 1, 4, COMP }, { "wl_seat", 3, 5, SEAT }, { "xdg_wm_base", 5, 3, WMB } };
    for (int i = 0; i < 3; i++)
      if (bind_global(REG, binds[i].name, binds[i].n, binds[i].ver, binds[i].nid) != 0) return 1;
  }
  drain(150);
  req(SEAT, 0, 1, (uint32_t)PTR);                                // wl_seat.get_pointer
  drain(100);

  // --- the toplevel --------------------------------------------------------
  req(COMP, 0, 1, (uint32_t)TOP_SURF);                           // create_surface
  req(WMB,  2, 2, (uint32_t)TOP_XDG, (uint32_t)TOP_SURF);        // get_xdg_surface
  req(TOP_XDG, 1, 1, (uint32_t)TOP_TL);                          // get_toplevel
  req(TOP_SURF, 6, 0);                                           // commit
  drain(150);
  req(TOP_XDG, 4, 1, 1u);                                        // ack_configure
  drain(100);

  // --- a SECOND wl_seat and a SECOND wl_pointer, which is what GTK4 really does ------------
  // Measured from gtk4-widget-factory under the headless harness: GTK4 opens three wl_registry
  // objects on ONE wl_display and binds wl_seat twice, so two wl_pointer resources exist at once
  // (get_pointer -> 7 at v5, then get_pointer -> 21 at v4). A compositor that keeps a single
  // g_ptr_id serves only the LAST one, and GTK4 — listening on the first — never sees another
  // event while the window keeps painting. That is the "renders fine, nothing is clickable" bug.
  // The second seat binds at v4 on purpose: a v4 wl_pointer gets no frame events, so this also
  // checks the per-resource version is honoured rather than one global version.
  if (bind_global(REG, 3, "wl_seat", 4, SEAT2) != 0) return 1;
  drain(100);
  req(SEAT2, 0, 1, (uint32_t)PTR2);                              // second wl_seat.get_pointer
  drain(100);

  evn = 0;                                                       // baseline: only measure below
  g_cx = 100; g_cy = 100; push(GPUWIN_EV_MOTION, 0, 0, 100, 100);
  drain(120);
  size_t before_popup = evn;

  // --- a popup, exactly as a GTK4 tooltip/popover does it ------------------
  req(WMB, 1, 1, (uint32_t)POS);                                 // create_positioner
  req(POS, 1, 2, 200u, 150u);                                    // set_size 200x150
  req(COMP, 0, 1, (uint32_t)POP_SURF);
  req(WMB,  2, 2, (uint32_t)POP_XDG, (uint32_t)POP_SURF);
  req(POP_XDG, 2, 3, (uint32_t)POP_POPUP, (uint32_t)TOP_XDG, (uint32_t)POS);   // get_popup
  req(POP_SURF, 6, 0);                                           // commit
  drain(150);
  req(POP_XDG, 4, 1, 2u);                                        // ack_configure
  drain(100);

  // ...and dismissed, which is where GTK4 destroys the popup's wl_surface.
  req(POP_POPUP, 0, 0);                                          // xdg_popup.destroy
  req(POP_XDG,   0, 0);                                          // xdg_surface.destroy
  req(POP_SURF,  0, 0);                                          // wl_surface.destroy
  drain(150);

  size_t after_popup = evn;

  // --- THE MEASUREMENT: move and click after the popup is gone -------------
  g_cx = 120; g_cy = 130; push(GPUWIN_EV_MOTION, 0, 0, 120, 130);
  drain(120);
  push(GPUWIN_EV_BUTTON, 0x110, 1, 120, 130);
  drain(120);
  push(GPUWIN_EV_BUTTON, 0x110, 0, 120, 130);
  drain(150);

  // --- SECOND MEASUREMENT: three host events inside ONE 16ms poll tick -----
  // A frame terminates a logical group, not a tick. Queue motion+press+release together and
  // count how many frames come back: three groups must produce three frames.
  size_t batch_start = evn;
  g_cx = 140; g_cy = 150;
  push(GPUWIN_EV_MOTION, 0, 0, 140, 150);
  push(GPUWIN_EV_BUTTON, 0x110, 1, 140, 150);
  push(GPUWIN_EV_BUTTON, 0x110, 0, 140, 150);
  drain(200);

  // --- decode every wl_pointer event addressed to PTR ----------------------
  printf("\n=== wl_pointer traffic ===\n");
  int enters = 0, buttons = 0, frames = 0, bad_surface = 0, ok_surface = 0;
  int enters2 = 0, buttons2 = 0, motions2 = 0, frames2 = 0;
  int batch_ev = 0, batch_frames = 0;
  size_t off = 0;
  int phase = 0;
  while (off + 8 <= evn) {
    uint32_t id = rd32(ev + off);
    uint32_t so = rd32(ev + off + 4);
    uint16_t op = so & 0xFFFF, sz = so >> 16;
    if (sz < 8 || off + sz > evn) break;
    if (off >= before_popup && phase == 0) { printf("  --- popup created+destroyed ---\n"); phase = 1; }
    if (off >= after_popup && phase == 1) { printf("  --- popup gone; move + click ---\n"); phase = 2; }
    if (off >= batch_start && phase == 2) { printf("  --- motion+press+release in ONE tick ---\n"); phase = 3; }
    if (phase == 3 && id == PTR) { batch_ev++; if (op == 5) batch_frames++; }
    if (id == PTR2) {
      if (op == 0) enters2++; else if (op == 2) motions2++;
      else if (op == 3) buttons2++; else if (op == 5) frames2++;
    }
    if (id == PTR) {
      printf("  wl_pointer.%-12s", pop(op));
      if (op == 0) {                                            // enter(serial, surface, x, y)
        uint32_t ser = rd32(ev + off + 8), surf = rd32(ev + off + 12);
        printf(" serial=%u surface=%u", ser, surf);
        enters++;
        if (surf == TOP_SURF) { ok_surface++; printf("   [live toplevel]"); }
        else { bad_surface++; printf("   [<<< NOT THE TOPLEVEL]"); }
      } else if (op == 1) {                                     // leave(serial, surface)
        printf(" serial=%u surface=%u", rd32(ev + off + 8), rd32(ev + off + 12));
      } else if (op == 3) {                                     // button(serial,time,btn,state)
        printf(" serial=%u btn=0x%x state=%u",
               rd32(ev + off + 8), rd32(ev + off + 16), rd32(ev + off + 20));
        buttons++;
      } else if (op == 5) frames++;
      printf("\n");
    }
    off += sz;
  }

  printf("\n=== verdict ===\n");
  printf("  enters=%d  (to live toplevel: %d, to a NON-toplevel/dead id: %d)\n",
         enters, ok_surface, bad_surface);
  printf("  buttons=%d  frames=%d\n", buttons, frames);
  printf("  SECOND wl_pointer@%u: enters=%d motions=%d buttons=%d frames=%d (v4: frames must be 0)\n",
         (unsigned)PTR2, enters2, motions2, buttons2, frames2);
  printf("  one-tick batch: %d pointer events terminated by %d frames (need 3)\n",
         batch_ev - batch_frames, batch_frames);
  // Both resources must be served, and equally: a client that holds two wl_pointers is entitled
  // to the same event stream on each.
  int both = (enters2 == enters) && (buttons2 == buttons) && (frames2 == 0);
  int pass = (bad_surface == 0) && (enters > 0) && (buttons == 4) && (batch_frames == 3) && both;
  printf("  RESULT: %s\n",
         !both ? "FAIL - a second wl_pointer exists and is NOT being served"
               : pass ? "PASS - input stays addressed to the live toplevel, on every wl_pointer"
                      : "FAIL - input is addressed at a surface GTK4 will discard");
  wlsrv_stop();
  unlink(sock);
  return pass ? 0 : 1;
}
