// test_wlflap.c — wl_pointer POSITION PUBLICATION, measured on the WIRE.
//
//   clang -arch arm64 -O1 -Isrc -o build/test_wlflap src/test_wlflap.c src/wlsrv.c \
//     && ./build/test_wlflap
//
// No Metal, no window, no display, no GUI lock. Exit status is the verdict.
//
// WHY THIS READS THE WIRE AND NOT THE LOG. ptr_leave() has no wlog() call, so a leave that WAS
// sent shows up as a missing line. Counting "leave 0" out of a log is therefore meaningless, and
// misreading it that way has already cost this investigation time. Everything below is counted
// by decoding the bytes the compositor actually wrote to the client socket.
//
// ---------------------------------------------------------------------------------------------
// CASE A — the hit target FLAPS while the cursor never moves.
//
// A popup maps and unmaps under a stationary cursor (a menu opening and closing). Each flap makes
// ptr_goto() take the `tgt != g_ptr_surface` branch: ptr_leave(), then ptr_enter_if_needed().
// The enter carries surface_x/surface_y, so the position IS published — as a stream of
// enter/leave pairs with ZERO motion events. This case exists to CHARACTERISE that, because it
// is what "enter 18, motion 0" in a drift-era log actually was. It is noisy but not lossy, so
// this case only reports; it does not fail the build.
//
// ---------------------------------------------------------------------------------------------
// CASE B — the real defect: a SECOND wl_pointer silently swallows the first one's motion.
//
// ptr_enter_if_needed() assigns g_sent_x/g_sent_y INSIDE its per-resource loop:
//
//     for (i...) {
//       if (g_ptrs[i].entered) continue;         // already knows where the pointer is
//       ...send enter(lx,ly) to g_ptrs[i]...
//       g_sent_x = lx; g_sent_y = ly;            // "the enter itself published the position"
//     }
//
// and ptr_goto() then does:
//
//     ptr_enter_if_needed(lx, ly);
//     if (lx == g_sent_x && ly == g_sent_y) return;   // nothing new to say
//
// That comment is only true when EVERY resource was entered by that call. When resource A is
// already entered and resource B is not, B's enter publishes the position to B alone, yet it
// sets the shared g_sent — so the dedup concludes there is "nothing new to say" and A, which
// was told nothing, never receives the motion. A's idea of the pointer position is stale from
// then on.
//
// This is not hypothetical: GTK4 binds wl_seat more than once on a single wl_display and ends up
// holding two wl_pointer resources (see the note in src/test_wlpointer.c, measured from
// gtk4-widget-factory). The second one is created while the pointer is already inside the
// toplevel, which is exactly the ordering that triggers this.
//
// Case B is the assertion that fails on a broken build.
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
// Move the cursor the way a real host does: the poll source of truth AND an event, together.
static void cursor_to(int x, int y) {
  g_cx = x; g_cy = y;
  push(GPUWIN_EV_MOTION, 0, 0, x, y);
}

// ------------------------------------------------------------- client plumbing
static int cfd;
static uint32_t rd32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void req(uint32_t id, uint16_t op, int argc, ...) {
  uint8_t b[256]; size_t n = 8;
  va_list ap; va_start(ap, argc);
  for (int i = 0; i < argc; i++) { wr32(b + n, va_arg(ap, uint32_t)); n += 4; }
  va_end(ap);
  wr32(b, id); wr32(b + 4, (uint32_t)op | ((uint32_t)n << 16));
  if (write(cfd, b, n) != (ssize_t)n) { perror("write"); exit(1); }
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

// wl_fixed -> int, for decoding enter/motion coordinates off the wire.
static int fixed_to_int(uint32_t v) { return (int)((int32_t)v / 256); }

// Count of each wl_pointer opcode, per resource, over a byte range of the capture.
struct tally { int enter, leave, motion, button, frame; int last_x, last_y; int have_xy; };

static void tally_range(size_t from, size_t to, uint32_t which, struct tally* t) {
  memset(t, 0, sizeof *t);
  size_t off = 0;
  while (off + 8 <= evn) {
    uint32_t id = rd32(ev + off);
    uint32_t so = rd32(ev + off + 4);
    uint16_t op = so & 0xFFFF, sz = so >> 16;
    if (sz < 8 || off + sz > evn) break;
    if (off >= from && off < to && id == which) {
      switch (op) {
        case 0: t->enter++;
                t->last_x = fixed_to_int(rd32(ev + off + 16));
                t->last_y = fixed_to_int(rd32(ev + off + 20));
                t->have_xy = 1; break;
        case 1: t->leave++;  break;
        case 2: t->motion++;                                    // motion(time, x, y)
                t->last_x = fixed_to_int(rd32(ev + off + 12));
                t->last_y = fixed_to_int(rd32(ev + off + 16));
                t->have_xy = 1; break;
        case 3: t->button++; break;
        case 5: t->frame++;  break;
        default: break;
      }
    }
    off += sz;
  }
}

static int g_fail;
static void ok(const char* what, int pass) {
  printf("  %-64s %s\n", what, pass ? "PASS" : "FAIL");
  if (!pass) g_fail++;
}

int main(int argc, char** argv) {
  const char* sock = argc > 1 ? argv[1] : "/tmp/d2-wlflap.sock";
  unlink(sock);
  if (wlsrv_start(sock) != 0) { fprintf(stderr, "wlsrv_start failed\n"); return 1; }
  usleep(100000);

  cfd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX; snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
  if (connect(cfd, (struct sockaddr*)&sa, sizeof sa) < 0) { perror("connect"); return 1; }

  enum { REG = 2, COMP = 3, SEAT = 4, WMB = 5, PTR = 6,
         TOP_SURF = 10, TOP_XDG = 11, TOP_TL = 12,
         POS = 20, POP_SURF = 21, POP_XDG = 22, POP_POPUP = 23,
         SEAT2 = 30, PTR2 = 31 };
  req(1, 1, 1, (uint32_t)REG);
  drain(150);
  { struct { const char* n; uint32_t name, ver, nid; } binds[] = {
      { "wl_compositor", 1, 4, COMP }, { "wl_seat", 3, 5, SEAT }, { "xdg_wm_base", 5, 3, WMB } };
    for (int i = 0; i < 3; i++)
      if (bind_global(REG, binds[i].name, binds[i].n, binds[i].ver, binds[i].nid) != 0) return 1;
  }
  drain(150);
  req(SEAT, 0, 1, (uint32_t)PTR);
  drain(100);

  req(COMP, 0, 1, (uint32_t)TOP_SURF);
  req(WMB,  2, 2, (uint32_t)TOP_XDG, (uint32_t)TOP_SURF);
  req(TOP_XDG, 1, 1, (uint32_t)TOP_TL);
  req(TOP_SURF, 6, 0);
  drain(150);
  req(TOP_XDG, 4, 1, 1u);
  drain(100);

  // Land the pointer inside the toplevel so it is already entered before anything below.
  cursor_to(400, 300);
  drain(150);

  // ============================================================ CASE A: flapping hit target
  printf("\n===== CASE A  popup maps/unmaps under a STATIONARY cursor =====\n");
  printf("  cursor pinned at (400,300) for the whole case; no motion input at all.\n");
  size_t a_from = evn;
  const int FLAPS = 6;
  for (int k = 0; k < FLAPS; k++) {
    // Map a popup that covers the cursor. set_size + a positioner anchored at the origin puts
    // it at (0,0); 600x500 comfortably contains (400,300).
    req(WMB, 1, 1, (uint32_t)POS);
    req(POS, 1, 2, 600u, 500u);                                  // set_size
    req(COMP, 0, 1, (uint32_t)POP_SURF);
    req(WMB,  2, 2, (uint32_t)POP_XDG, (uint32_t)POP_SURF);
    req(POP_XDG, 2, 3, (uint32_t)POP_POPUP, (uint32_t)TOP_XDG, (uint32_t)POS);
    req(POP_SURF, 6, 0);
    drain(80);
    req(POP_XDG, 4, 1, (uint32_t)(100 + k));                     // ack_configure
    drain(80);
    // ...and dismiss it again. Cursor STILL has not moved.
    req(POP_POPUP, 0, 0);
    req(POP_XDG,   0, 0);
    req(POP_SURF,  0, 0);
    drain(80);
  }
  size_t a_to = evn;
  struct tally a; tally_range(a_from, a_to, PTR, &a);
  printf("  %d popup map/unmap cycles, cursor never moved:\n", FLAPS);
  printf("    enter=%d  leave=%d  motion=%d  frame=%d\n", a.enter, a.leave, a.motion, a.frame);
  if (a.enter == 0 && a.leave == 0 && a.motion == 0) {
    printf("\n  INCONCLUSIVE — and the zeros say why. A surface is only `mapped` once a BUFFER\n"
           "  has been attached and committed; these popups are buffer-less, so hit_surface()\n"
           "  skipped them (`if (!s->used || !s->xdg_popup || !s->mapped) continue`), the hit\n"
           "  target never left the toplevel, and no flap ever happened. This case did NOT\n"
           "  exercise the flapping path and must not be quoted as evidence either way.\n"
           "  To make it real, port the wl_shm pool + attach recipe from src/test_wlpopup.c\n"
           "  (create_pool via SCM_RIGHTS, create_buffer, attach, commit). Left undone here\n"
           "  deliberately: CASE B below is a demonstrated defect and did not need it.\n");
  } else {
    printf("  -> position republished via ENTER, motion=%d. Each enter carries\n", a.motion);
    printf("     surface_x/surface_y, so this is noisy rather than lossy. REPORTED, not asserted.\n");
  }

  // ============================================================ CASE B: the real defect
  printf("\n===== CASE B  a SECOND wl_pointer created while the pointer is already inside =====\n");
  // GTK4 does exactly this: binds wl_seat again on the same wl_display and gets a second
  // wl_pointer, after the pointer has already entered the toplevel.
  if (bind_global(REG, 3, "wl_seat", 4, SEAT2) != 0) return 1;
  drain(100);
  req(SEAT2, 0, 1, (uint32_t)PTR2);
  drain(100);

  // One plain motion to a NEW point. PTR is already entered and needs a motion to learn it.
  // PTR2 is not entered and will get an enter. Both must end up knowing (455, 333).
  size_t b_from = evn;
  cursor_to(455, 333);
  drain(200);
  size_t b_to = evn;

  struct tally b1, b2;
  tally_range(b_from, b_to, PTR,  &b1);
  tally_range(b_from, b_to, PTR2, &b2);

  printf("  cursor moved (400,300) -> (455,333), one motion, no popups.\n");
  printf("    wl_pointer@%u (already entered): enter=%d motion=%d  last position (%d,%d)\n",
         (unsigned)PTR, b1.enter, b1.motion, b1.last_x, b1.last_y);
  printf("    wl_pointer@%u (just created)   : enter=%d motion=%d  last position (%d,%d)\n",
         (unsigned)PTR2, b2.enter, b2.motion, b2.last_x, b2.last_y);

  ok("the new wl_pointer is told the position (via enter)",
     b2.have_xy && b2.last_x == 455 && b2.last_y == 333);
  ok("the ALREADY-ENTERED wl_pointer receives a motion for the new point",
     b1.motion >= 1);
  ok("the ALREADY-ENTERED wl_pointer's position is not stale",
     b1.have_xy && b1.last_x == 455 && b1.last_y == 333);

  if (b1.motion == 0)
    printf("\n  ^ wl_pointer@%u got NOTHING. ptr_enter_if_needed() set g_sent_x/g_sent_y while\n"
           "    entering wl_pointer@%u, so ptr_goto()'s dedup decided there was 'nothing new to\n"
           "    say' and skipped the motion the first resource was owed.\n",
           (unsigned)PTR, (unsigned)PTR2);

  // ============================================================ CASE C: what it actually costs
  // The window of staleness is what matters, so measure it rather than assuming. A click is the
  // case that hurts: wl_pointer.button carries NO coordinates, so the client acts on the last
  // position it was told. If that is stale, the click lands somewhere the user did not click.
  printf("\n===== CASE C  a click delivered while the position is stale =====\n");
  size_t c_from = evn;
  push(GPUWIN_EV_BUTTON, 0x110, 1, 455, 333);
  push(GPUWIN_EV_BUTTON, 0x110, 0, 455, 333);
  drain(200);
  struct tally c1; tally_range(c_from, evn, PTR, &c1);
  printf("  press+release at the TRUE cursor point (455,333).\n");
  printf("    wl_pointer@%u: button=%d motion=%d\n", (unsigned)PTR, c1.button, c1.motion);
  printf("    the position wl_pointer@%u had been told, at the moment of the click: (%d,%d)\n",
         (unsigned)PTR, b1.have_xy ? b1.last_x : 400, b1.have_xy ? b1.last_y : 300);
  ok("the click is preceded by a motion that corrects the stale position",
     c1.button >= 1 && (c1.motion >= 1 || (b1.have_xy && b1.last_x == 455 && b1.last_y == 333)));
  if (c1.button >= 1 && c1.motion == 0 && !(b1.have_xy && b1.last_x == 455))
    printf("  ^ the button arrives with the client still believing the pointer is at (400,300).\n"
           "    wl_pointer.button carries no coordinates, so this click lands 55x33 px away from\n"
           "    where it happened — the 'the click did not land where I clicked' failure.\n");

  // Does it self-heal? Bounding the damage matters as much as finding it.
  printf("\n===== CASE D  does the staleness persist, or self-heal on the next move? =====\n");
  size_t d_from = evn;
  cursor_to(470, 340);
  drain(200);
  struct tally d1, d2;
  tally_range(d_from, evn, PTR,  &d1);
  tally_range(d_from, evn, PTR2, &d2);
  printf("    wl_pointer@%u: motion=%d last (%d,%d)\n", (unsigned)PTR,  d1.motion, d1.last_x, d1.last_y);
  printf("    wl_pointer@%u: motion=%d last (%d,%d)\n", (unsigned)PTR2, d2.motion, d2.last_x, d2.last_y);
  ok("both wl_pointers track the follow-up move",
     d1.motion >= 1 && d2.motion >= 1 &&
     d1.last_x == 470 && d1.last_y == 340 && d2.last_x == 470 && d2.last_y == 340);
  if (d1.motion >= 1)
    printf("  -> SELF-HEALS on the next move. The damage is bounded: exactly one lost motion,\n"
           "     at the moment a second wl_pointer appears. Not a permanent desync — but any\n"
           "     BUTTON landing inside that window (case C) is delivered at the stale point.\n");

  printf("\n===== verdict =====\n");
  printf("  %s\n", g_fail ? "FAIL — a wl_pointer is being left with a stale position"
                          : "PASS — every wl_pointer is told where the pointer is");
  wlsrv_stop();
  unlink(sock);
  return g_fail ? 1 : 0;
}
