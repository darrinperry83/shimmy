// test_wlpopup.c — headless xdg_popup harness for the in-process compositor.
//
//   clang -arch arm64 -O1 -Isrc -o build/test_wlpopup src/test_wlpopup.c src/wlsrv.c \
//     && ./build/test_wlpopup
//
// No Metal, no window, no display, and NO GUI LOCK: the host-window and renderer entry points are
// stubbed, and the renderer stub keeps the presented framebuffer so the composited result can be
// inspected in pixels. Exit status is the verdict.
//
// This is the acceptance test for "a GTK4 menu opens invisibly and cannot be used", reduced to
// the protocol traffic a toolkit actually emits. It answers five questions that a log cannot:
//
//   1. Is the popup DRAWN?  Its pixels must appear in the presented frame, over the toplevel's.
//   2. Is it drawn WHERE THE POSITIONER ASKED?  anchor rect + anchor + gravity + offset.
//   3. Does the host window keep the TOPLEVEL's size?  A popup must never resize it.
//   4. Is the popup HIT-TESTED, in its OWN coordinates?  enter must name the popup surface and
//      carry surface-local x/y, not window-global ones.
//   5. Does a press outside it send xdg_popup.popup_done, and does destroying it repaint?
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include "gpu.h"
#include "wlsrv.h"

// ---------------------------------------------------------------- host stubs
static struct gpuwin_event g_q[64];
static int g_qh, g_qt;
static int g_cx, g_cy;

// The renderer stub RETAINS the last presented frame. That is the whole point: compositing is a
// pixel question and there is no screen here to look at.
static uint8_t* g_fb;
static uint32_t g_fb_w, g_fb_h;
static int      g_opens, g_presents;
static uint32_t g_open_w, g_open_h;

void x11rnd_open(uint32_t w, uint32_t h) { g_opens++; g_open_w = w; g_open_h = h; }
void x11rnd_present(const uint8_t* rgba, uint32_t w, uint32_t h) {
  size_t n = (size_t)w * h * 4;
  g_fb = realloc(g_fb, n);
  memcpy(g_fb, rgba, n);
  g_fb_w = w; g_fb_h = h; g_presents++;
}
void x11rnd_present_bgra(const uint8_t* p, uint32_t w, uint32_t h) { x11rnd_present(p, w, h); }
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
// Pixel at (x,y) of the presented frame, as 0xRRGGBB.
static uint32_t fbpx(int x, int y) {
  if (!g_fb || x < 0 || y < 0 || (uint32_t)x >= g_fb_w || (uint32_t)y >= g_fb_h) return 0xDEAD;
  const uint8_t* p = g_fb + ((size_t)y * g_fb_w + (size_t)x) * 4;
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

// ------------------------------------------------------------- client plumbing
static int cfd;
static uint32_t rd32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t* p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void req(uint32_t id, uint16_t op, int argc, ...) {
  uint8_t b[256]; size_t n = 8;
  va_list ap; va_start(ap, argc);
  for (int i = 0; i < argc; i++) { wr32(b + n, va_arg(ap, uint32_t)); n += 4; }
  va_end(ap);
  wr32(b, id); wr32(b + 4, (uint32_t)op | ((uint32_t)n << 16));
  if (write(cfd, b, n) != (ssize_t)n) { perror("write"); exit(1); }
}
// wl_shm.create_pool needs the fd out of band, in SCM_RIGHTS, exactly as libwayland sends it.
static void req_fd(uint32_t id, uint16_t op, int fd, int argc, ...) {
  uint8_t b[256]; size_t n = 8;
  va_list ap; va_start(ap, argc);
  for (int i = 0; i < argc; i++) { wr32(b + n, va_arg(ap, uint32_t)); n += 4; }
  va_end(ap);
  wr32(b, id); wr32(b + 4, (uint32_t)op | ((uint32_t)n << 16));

  struct iovec io = { b, n };
  union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } cm;
  memset(&cm, 0, sizeof cm);
  struct msghdr m; memset(&m, 0, sizeof m);
  m.msg_iov = &io; m.msg_iovlen = 1;
  m.msg_control = cm.buf; m.msg_controllen = CMSG_SPACE(sizeof(int));
  struct cmsghdr* c = CMSG_FIRSTHDR(&m);
  c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &fd, sizeof(int));
  if (sendmsg(cfd, &m, 0) < 0) { perror("sendmsg"); exit(1); }
}

static uint8_t ev[262144];
static size_t evn;
static void drain(int ms) {
  struct pollfd p = { cfd, POLLIN, 0 };
  while (poll(&p, 1, ms) > 0 && (p.revents & POLLIN)) {
    ssize_t r = read(cfd, ev + evn, sizeof ev - evn);
    if (r <= 0) break;
    evn += (size_t)r;
    ms = 30;
  }
}
// Was an event with this (object, opcode) seen at or after `from`?
static int saw(size_t from, uint32_t want_id, uint16_t want_op) {
  size_t off = 0;
  while (off + 8 <= evn) {
    uint32_t id = rd32(ev + off);
    uint32_t so = rd32(ev + off + 4);
    uint16_t op = so & 0xFFFF, sz = so >> 16;
    if (sz < 8 || off + sz > evn) break;
    if (off >= from && id == want_id && op == want_op) return 1;
    off += sz;
  }
  return 0;
}
// Most recent wl_pointer.enter at or after `from`: which surface, and at what surface-local x/y.
static int last_enter(size_t from, uint32_t ptr, uint32_t* surf, int32_t* x, int32_t* y) {
  size_t off = 0; int found = 0;
  while (off + 8 <= evn) {
    uint32_t id = rd32(ev + off);
    uint32_t so = rd32(ev + off + 4);
    uint16_t op = so & 0xFFFF, sz = so >> 16;
    if (sz < 8 || off + sz > evn) break;
    if (off >= from && id == ptr && op == 0 && off + 24 <= evn) {
      *surf = rd32(ev + off + 12);
      *x = (int32_t)rd32(ev + off + 16) / 256;
      *y = (int32_t)rd32(ev + off + 20) / 256;
      found = 1;
    }
    off += sz;
  }
  return found;
}

// A pool big enough for both buffers, filled with two flat colours.
#define TOP_W 800
#define TOP_H 600
#define POP_W 200
#define POP_H 150
#define TOP_RGB 0x204080u
#define POP_RGB 0xC03010u
static uint8_t* g_pool_map;
static size_t   g_top_off, g_pop_off, g_pool_size;

static void fill(size_t off, int w, int h, uint32_t rgb) {
  // wl_shm XRGB8888 is a little-endian 0x00RRGGBB word: B,G,R,X in memory.
  for (int i = 0; i < w * h; i++) {
    uint8_t* p = g_pool_map + off + (size_t)i * 4;
    p[0] = rgb & 0xFF; p[1] = (rgb >> 8) & 0xFF; p[2] = (rgb >> 16) & 0xFF; p[3] = 0xFF;
  }
}

int main(int argc, char** argv) {
  const char* sock = argc > 1 ? argv[1] : "/tmp/m1-wlpopup-harness.sock";
  unlink(sock);
  if (wlsrv_start(sock) != 0) { fprintf(stderr, "wlsrv_start failed\n"); return 1; }
  usleep(100000);

  cfd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX; snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
  if (connect(cfd, (struct sockaddr*)&sa, sizeof sa) < 0) { perror("connect"); return 1; }

  // --- a real shm pool, because compositing is about pixels ------------------
  g_top_off = 0;
  g_pop_off = (size_t)TOP_W * TOP_H * 4;
  g_pool_size = g_pop_off + (size_t)POP_W * POP_H * 4;
  char tmpl[] = "/tmp/m1-wlpopup-poolXXXXXX";
  int pfd = mkstemp(tmpl);
  if (pfd < 0) { perror("mkstemp"); return 1; }
  unlink(tmpl);
  if (ftruncate(pfd, (off_t)g_pool_size) != 0) { perror("ftruncate"); return 1; }
  g_pool_map = mmap(NULL, g_pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, pfd, 0);
  if (g_pool_map == MAP_FAILED) { perror("mmap"); return 1; }
  fill(g_top_off, TOP_W, TOP_H, TOP_RGB);
  fill(g_pop_off, POP_W, POP_H, POP_RGB);

  enum { REG = 2, COMP = 3, SEAT = 4, WMB = 5, PTR = 6, SHM = 7,
         POOL = 8, TOP_BUF = 9, POP_BUF = 15,
         TOP_SURF = 10, TOP_XDG = 11, TOP_TL = 12,
         POS = 20, POP_SURF = 21, POP_XDG = 22, POP_POPUP = 23 };
  req(1, 1, 1, (uint32_t)REG);                                   // wl_display.get_registry
  drain(150);

  { struct { const char* n; uint32_t name, ver, nid; } binds[] = {
      { "wl_compositor", 1, 4, COMP }, { "wl_seat", 3, 5, SEAT },
      { "xdg_wm_base", 5, 3, WMB },    { "wl_shm", 2, 1, SHM } };
    for (int i = 0; i < 4; i++) {
      uint8_t b[128]; size_t n = 8;
      wr32(b + n, binds[i].name); n += 4;
      size_t l = strlen(binds[i].n) + 1;
      wr32(b + n, (uint32_t)l); n += 4;
      memcpy(b + n, binds[i].n, l); n += l;
      while (n & 3) b[n++] = 0;
      wr32(b + n, binds[i].ver); n += 4;
      wr32(b + n, binds[i].nid); n += 4;
      wr32(b, REG); wr32(b + 4, 0u | ((uint32_t)n << 16));
      if (write(cfd, b, n) != (ssize_t)n) { perror("bind"); return 1; }
    }
  }
  drain(150);
  req(SEAT, 0, 1, (uint32_t)PTR);                                // wl_seat.get_pointer
  req_fd(SHM, 0, pfd, 2, (uint32_t)POOL, (uint32_t)g_pool_size); // wl_shm.create_pool
  drain(150);
  req(POOL, 0, 6, (uint32_t)TOP_BUF, (uint32_t)g_top_off,
      (uint32_t)TOP_W, (uint32_t)TOP_H, (uint32_t)(TOP_W * 4), 1u);   // 1 = XRGB8888
  req(POOL, 0, 6, (uint32_t)POP_BUF, (uint32_t)g_pop_off,
      (uint32_t)POP_W, (uint32_t)POP_H, (uint32_t)(POP_W * 4), 1u);
  drain(150);

  // --- the toplevel, painted -----------------------------------------------
  req(COMP, 0, 1, (uint32_t)TOP_SURF);                           // create_surface
  req(WMB,  2, 2, (uint32_t)TOP_XDG, (uint32_t)TOP_SURF);        // get_xdg_surface
  req(TOP_XDG, 1, 1, (uint32_t)TOP_TL);                          // get_toplevel
  req(TOP_SURF, 6, 0);                                           // commit (buffer-less)
  drain(200);
  req(TOP_XDG, 4, 1, 1u);                                        // ack_configure
  req(TOP_SURF, 1, 3, (uint32_t)TOP_BUF, 0u, 0u);                // attach
  req(TOP_SURF, 6, 0);                                           // commit
  drain(250);

  printf("=== 1. toplevel ===\n");
  printf("  presented %dx%d after %d present(s); x11rnd_open(%u,%u)\n",
         g_fb_w, g_fb_h, g_presents, g_open_w, g_open_h);
  int top_ok = (g_fb_w == TOP_W && g_fb_h == TOP_H && fbpx(10, 10) == TOP_RGB);
  printf("  pixel(10,10)=0x%06X (want 0x%06X)  -> %s\n",
         fbpx(10, 10), TOP_RGB, top_ok ? "ok" : "BAD");

  // --- a popup, positioned the way a GTK menu positions one -----------------
  // Anchor rect (400,300) 40x20 in the toplevel; anchor=bottom(2), gravity=bottom(2), offset(5,7).
  //   anchor point = (400 + 40/2, 300 + 20) = (420, 320)
  //   gravity bottom keeps y and centres x: (420 - 200/2, 320) = (320, 320)
  //   + offset                            = (325, 327)
  const int WANT_X = 325, WANT_Y = 327;
  req(WMB, 1, 1, (uint32_t)POS);                                 // create_positioner
  req(POS, 1, 2, (uint32_t)POP_W, (uint32_t)POP_H);              // set_size
  req(POS, 2, 4, 400u, 300u, 40u, 20u);                          // set_anchor_rect
  req(POS, 3, 1, 2u);                                            // set_anchor  = bottom
  req(POS, 4, 1, 2u);                                            // set_gravity = bottom
  req(POS, 6, 2, 5u, 7u);                                        // set_offset
  req(COMP, 0, 1, (uint32_t)POP_SURF);
  req(WMB,  2, 2, (uint32_t)POP_XDG, (uint32_t)POP_SURF);
  req(POP_XDG, 2, 3, (uint32_t)POP_POPUP, (uint32_t)TOP_XDG, (uint32_t)POS);   // get_popup
  req(POP_SURF, 6, 0);                                           // commit (buffer-less)
  drain(200);
  req(POP_XDG, 4, 1, 2u);                                        // ack_configure
  req(POP_SURF, 1, 3, (uint32_t)POP_BUF, 0u, 0u);                // attach
  req(POP_SURF, 6, 0);                                           // commit -> should COMPOSITE
  drain(250);

  printf("\n=== 2. popup composited ===\n");
  printf("  window is still %dx%d (must stay %dx%d — a popup may not resize it)\n",
         g_fb_w, g_fb_h, TOP_W, TOP_H);
  int size_ok = (g_fb_w == TOP_W && g_fb_h == TOP_H);
  uint32_t c_in  = fbpx(WANT_X + POP_W / 2, WANT_Y + POP_H / 2);   // centre of where it should be
  uint32_t c_tl  = fbpx(WANT_X, WANT_Y);                           // its top-left corner
  uint32_t c_out = fbpx(WANT_X - 5, WANT_Y - 5);                   // just outside it
  uint32_t c_br  = fbpx(WANT_X + POP_W - 1, WANT_Y + POP_H - 1);   // bottom-right corner
  uint32_t c_past= fbpx(WANT_X + POP_W, WANT_Y + POP_H);           // one past it
  printf("  popup expected at (%d,%d) %dx%d\n", WANT_X, WANT_Y, POP_W, POP_H);
  printf("    centre       0x%06X (want popup 0x%06X)\n", c_in,  POP_RGB);
  printf("    top-left     0x%06X (want popup 0x%06X)\n", c_tl,  POP_RGB);
  printf("    bottom-right 0x%06X (want popup 0x%06X)\n", c_br,  POP_RGB);
  printf("    just outside 0x%06X (want toplevel 0x%06X)\n", c_out, TOP_RGB);
  printf("    one past     0x%06X (want toplevel 0x%06X)\n", c_past, TOP_RGB);
  int draw_ok = (c_in == POP_RGB && c_tl == POP_RGB && c_br == POP_RGB &&
                 c_out == TOP_RGB && c_past == TOP_RGB);

  // --- hit-testing into the popup, in the POPUP's own coordinates ----------
  printf("\n=== 3. hit-testing ===\n");
  size_t mark = evn;
  int hx = WANT_X + 30, hy = WANT_Y + 40;         // 30,40 INSIDE the popup
  g_cx = hx; g_cy = hy; push(GPUWIN_EV_MOTION, 0, 0, hx, hy);
  drain(200);
  uint32_t esurf = 0; int ex = -1, ey = -1;
  int got = last_enter(mark, PTR, &esurf, &ex, &ey);
  printf("  cursor at window (%d,%d) -> enter surface=%u at (%d,%d)\n", hx, hy, esurf, ex, ey);
  printf("    want surface=%u (the POPUP) at (30,40) — SURFACE-LOCAL, not window-global\n",
         (uint32_t)POP_SURF);
  int hit_ok = got && esurf == POP_SURF && ex == 30 && ey == 40;

  // A press inside the popup must NOT dismiss it.
  mark = evn;
  push(GPUWIN_EV_BUTTON, 0x110, 1, hx, hy);
  push(GPUWIN_EV_BUTTON, 0x110, 0, hx, hy);
  drain(200);
  int inside_kept = !saw(mark, POP_POPUP, 1);      // xdg_popup.popup_done is opcode 1
  printf("  press INSIDE the popup -> popup_done sent? %s (must be no)\n",
         inside_kept ? "no" : "YES");

  // --- a press outside dismisses it ---------------------------------------
  printf("\n=== 4. dismissal ===\n");
  mark = evn;
  g_cx = 50; g_cy = 50; push(GPUWIN_EV_MOTION, 0, 0, 50, 50);
  push(GPUWIN_EV_BUTTON, 0x110, 1, 50, 50);
  push(GPUWIN_EV_BUTTON, 0x110, 0, 50, 50);
  drain(200);
  int done_ok = saw(mark, POP_POPUP, 1);
  printf("  press OUTSIDE at (50,50) -> xdg_popup.popup_done sent? %s (must be yes)\n",
         done_ok ? "yes" : "NO");
  uint32_t esurf2 = 0; int ex2 = 0, ey2 = 0;
  int got2 = last_enter(mark, PTR, &esurf2, &ex2, &ey2);
  printf("  pointer moved back to surface=%u at (%d,%d) (want the toplevel %u at 50,50)\n",
         esurf2, ex2, ey2, (uint32_t)TOP_SURF);
  int back_ok = got2 && esurf2 == TOP_SURF && ex2 == 50 && ey2 == 50;

  // ...and the client destroys it, which must repaint the hole.
  req(POP_POPUP, 0, 0);                                          // xdg_popup.destroy
  req(POP_XDG,   0, 0);                                          // xdg_surface.destroy
  req(POP_SURF,  0, 0);                                          // wl_surface.destroy
  drain(250);
  uint32_t after = fbpx(WANT_X + POP_W / 2, WANT_Y + POP_H / 2);
  printf("  after destroy, pixel where the popup was = 0x%06X (want toplevel 0x%06X)\n",
         after, TOP_RGB);
  int gone_ok = (after == TOP_RGB) && (g_fb_w == TOP_W && g_fb_h == TOP_H);

  printf("\n=== verdict ===\n");
  printf("  toplevel presented ................ %s\n", top_ok      ? "PASS" : "FAIL");
  printf("  popup DRAWN at the anchored rect .. %s\n", draw_ok     ? "PASS" : "FAIL");
  printf("  window size untouched by popup .... %s\n", size_ok     ? "PASS" : "FAIL");
  printf("  hit-test, surface-local coords .... %s\n", hit_ok      ? "PASS" : "FAIL");
  printf("  press inside does not dismiss ..... %s\n", inside_kept ? "PASS" : "FAIL");
  printf("  press outside sends popup_done .... %s\n", done_ok     ? "PASS" : "FAIL");
  printf("  pointer returns to the toplevel ... %s\n", back_ok     ? "PASS" : "FAIL");
  printf("  destroy repaints the hole ......... %s\n", gone_ok     ? "PASS" : "FAIL");
  int pass = top_ok && draw_ok && size_ok && hit_ok && inside_kept && done_ok && back_ok && gone_ok;
  printf("  RESULT: %s\n", pass ? "PASS - a popup is visible, clickable and dismissable"
                                : "FAIL");
  wlsrv_stop();
  unlink(sock);
  return pass ? 0 : 1;
}
