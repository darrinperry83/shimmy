// test_x11srv.c — standalone conformance test for the in-process X11 server.
//
// Builds without Metal / Cocoa / the rest of the shim: the render bridge
// (x11rnd_*) and the live cursor (x11srv_pointer) are stubbed right here, so the
// server core can be exercised on its own.
//
//   clang -arch arm64 -O2 -o test_x11srv test_x11srv.c x11srv.c && ./test_x11srv
//
// It hand-rolls the client side of the X11 wire protocol (no libxcb/libX11
// dependency), validates every reply's opcode / sequence / length, and dumps the
// presented framebuffer to tests/x11srv_frame.png so the raster can be eyeballed.
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <time.h>

#include "x11srv.h"

// Optional: cross-check against the REAL client library.  Build with
//   clang -arch arm64 -O2 -DHAVE_XCB -I/opt/homebrew/opt/libxcb/include \
//         -o test_x11srv test_x11srv.c x11srv.c \
//         -L/opt/homebrew/opt/libxcb/lib -lxcb -lxcb-glx
// xcb aborts with "Too much data requested from _XRead" on any reply whose
// length field disagrees with its computed list lengths, so this is the
// authoritative check for the setup reply and for GLX GetFBConfigs.
#ifdef HAVE_XCB
#include <xcb/xcb.h>
#include <xcb/glx.h>
#endif
// Optional: the same again through libX11, which is what Xt/Xaw (xeyes) actually uses.
//   ... -DHAVE_XLIB -I/opt/homebrew/opt/libx11/include -L/opt/homebrew/opt/libx11/lib -lX11
#ifdef HAVE_XLIB
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

#define SOCK_PATH "/Users/dperry/linux-shim/tests/x11srv_test.sock"
#define PNG_PATH  "/Users/dperry/linux-shim/tests/x11srv_frame.png"

// ---------------------------------------------------------------------------
// stubs for the render bridge + live cursor
// ---------------------------------------------------------------------------
static pthread_mutex_t g_cap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t* g_cap; static uint32_t g_cap_w, g_cap_h; static int g_cap_n;
static uint32_t g_open_w, g_open_h; static int g_open_n;

void x11rnd_open(uint32_t w, uint32_t h) {
  pthread_mutex_lock(&g_cap_lock);
  g_open_w = w; g_open_h = h; g_open_n++;
  pthread_mutex_unlock(&g_cap_lock);
}
void x11rnd_present_bgra(const uint8_t* p, uint32_t w, uint32_t h) { x11rnd_present(p, w, h); }
void x11rnd_present(const uint8_t* rgba, uint32_t w, uint32_t h) {
  pthread_mutex_lock(&g_cap_lock);
  size_t n = (size_t)w * h * 4;
  free(g_cap);
  g_cap = (uint8_t*)malloc(n);
  if (g_cap) memcpy(g_cap, rgba, n);
  g_cap_w = w; g_cap_h = h; g_cap_n++;
  pthread_mutex_unlock(&g_cap_lock);
}
int x11rnd_poll(void) { return 0; }

#define CURSOR_X 123
#define CURSOR_Y 45
void x11srv_pointer(int* x, int* y) { if (x) *x = CURSOR_X; if (y) *y = CURSOR_Y; }

// ---------------------------------------------------------------------------
// assertions
// ---------------------------------------------------------------------------
static int g_pass, g_fail;
static void check(int ok, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  printf("  %s  ", ok ? "PASS" : "FAIL");
  vprintf(fmt, ap);
  putchar('\n');
  va_end(ap);
  if (ok) g_pass++; else g_fail++;
}
static void section(const char* s) { printf("\n== %s ==\n", s); }

// ---------------------------------------------------------------------------
// wire helpers
// ---------------------------------------------------------------------------
static int  g_fd = -1;
static uint32_t g_cseq;                                  // client-side sequence counter

static uint16_t r16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr(const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  size_t o = 0;
  while (o < n) {
    ssize_t k = write(g_fd, b + o, n - o);
    if (k <= 0) { if (errno == EINTR) continue; fprintf(stderr, "write failed\n"); exit(2); }
    o += (size_t)k;
  }
}
static int rd_exact(void* p, size_t n, int timeout_ms) {
  uint8_t* b = (uint8_t*)p;
  size_t o = 0;
  while (o < n) {
    struct pollfd pf = { g_fd, POLLIN, 0 };
    int nr = poll(&pf, 1, timeout_ms);
    if (nr <= 0) return 0;
    ssize_t k = read(g_fd, b + o, n - o);
    if (k <= 0) { if (k < 0 && errno == EINTR) continue; return 0; }
    o += (size_t)k;
  }
  return 1;
}

// send a request; returns the sequence number the server will stamp on the reply
static uint32_t req(uint8_t op, uint8_t detail, const void* data, size_t dlen) {
  if (dlen & 3) { fprintf(stderr, "req %u: dlen %zu not a multiple of 4\n", op, dlen); exit(2); }
  uint8_t h[4];
  h[0] = op; h[1] = detail;
  w16(h + 2, (uint16_t)((dlen + 4) / 4));
  wr(h, 4);
  if (dlen) wr(data, dlen);
  return ++g_cseq;
}

// BIG-REQUESTS form: the 16-bit length is 0 and a CARD32 unit count follows the header
static uint32_t req_big(uint8_t op, uint8_t detail, const void* data, size_t dlen) {
  if (dlen & 3) { fprintf(stderr, "req_big %u: bad dlen\n", op); exit(2); }
  uint8_t h[8];
  h[0] = op; h[1] = detail;
  w16(h + 2, 0);
  w32(h + 4, (uint32_t)((dlen + 8) / 4));
  wr(h, 8);
  if (dlen) wr(data, dlen);
  return ++g_cseq;
}

// event queue
#define MAXEV 256
static uint8_t g_ev[MAXEV][32]; static int g_nev;

// Reads one server message. Returns 1 reply, 2 event, 3 error, 0 timeout.
static int read_msg(uint8_t hdr[32], uint8_t** tail, size_t* tlen, int timeout_ms) {
  if (tail) *tail = NULL;
  if (tlen) *tlen = 0;
  if (!rd_exact(hdr, 32, timeout_ms)) return 0;
  if (hdr[0] == 1) {
    size_t n = (size_t)r32(hdr + 4) * 4;
    if (n) {
      uint8_t* t = (uint8_t*)malloc(n);
      if (!rd_exact(t, n, 2000)) { free(t); return 0; }
      if (tail) *tail = t; else free(t);
      if (tlen) *tlen = n;
    }
    return 1;
  }
  if (hdr[0] == 0) return 3;
  if (g_nev < MAXEV) memcpy(g_ev[g_nev++], hdr, 32);
  return 2;
}

// waits for the reply to `seq`, queueing any events that arrive first
static int expect_reply(uint32_t seq, uint8_t hdr[32], uint8_t** tail, size_t* tlen, const char* what) {
  for (int i = 0; i < 64; i++) {
    int k = read_msg(hdr, tail, tlen, 2000);
    if (k == 0) { check(0, "%s: TIMED OUT waiting for a reply (server hang!)", what); return 0; }
    if (k == 2) continue;
    if (k == 3) {
      check(0, "%s: server returned error code %u (bad=0x%x major=%u)",
            what, hdr[1], r32(hdr + 4), hdr[10]);
      return 0;
    }
    if (r16(hdr + 2) != (uint16_t)seq) {
      check(0, "%s: reply sequence %u != expected %u", what, r16(hdr + 2), seq);
      return 0;
    }
    return 1;
  }
  return 0;
}

static void drain(int ms) {
  uint8_t hdr[32];
  for (;;) {
    int k = read_msg(hdr, NULL, NULL, ms);
    if (k == 0) return;
    if (k == 3) printf("  note  unexpected error: code %u major %u\n", hdr[1], hdr[10]);
  }
}
static int count_events(uint8_t code, uint32_t win) {
  int n = 0;
  for (int i = 0; i < g_nev; i++)
    if (g_ev[i][0] == code && r32(g_ev[i] + 4) == win) n++;
  return n;
}

// ---------------------------------------------------------------------------
// PNG writer (stored-deflate; no zlib dependency)
// ---------------------------------------------------------------------------
static uint32_t g_crc_tab[256]; static int g_crc_ready;
static uint32_t crc32b(const uint8_t* b, size_t n, uint32_t crc) {
  if (!g_crc_ready) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      g_crc_tab[i] = c;
    }
    g_crc_ready = 1;
  }
  crc ^= 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) crc = g_crc_tab[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}
static void be32(uint8_t* p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }

static void png_chunk(FILE* f, const char* type, const uint8_t* data, size_t n) {
  uint8_t hdr[8];
  be32(hdr, (uint32_t)n);
  memcpy(hdr + 4, type, 4);
  fwrite(hdr, 1, 8, f);
  if (n) fwrite(data, 1, n, f);
  uint32_t crc = crc32b((const uint8_t*)type, 4, 0);
  if (n) crc = crc32b(data, n, crc);
  uint8_t c[4]; be32(c, crc);
  fwrite(c, 1, 4, f);
}
static int png_write(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
  FILE* f = fopen(path, "wb");
  if (!f) return -1;
  static const uint8_t sig[8] = { 137,'P','N','G','\r','\n',26,'\n' };
  fwrite(sig, 1, 8, f);

  uint8_t ihdr[13];
  be32(ihdr, w); be32(ihdr + 4, h);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   // RGBA8
  png_chunk(f, "IHDR", ihdr, sizeof ihdr);

  size_t raw_n = (size_t)h * (1 + (size_t)w * 4);
  uint8_t* raw = (uint8_t*)malloc(raw_n);
  for (uint32_t y = 0; y < h; y++) {
    uint8_t* row = raw + (size_t)y * (1 + (size_t)w * 4);
    row[0] = 0;
    memcpy(row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
  }
  // zlib stream with stored deflate blocks
  size_t zcap = raw_n + (raw_n / 65535 + 1) * 5 + 16;
  uint8_t* z = (uint8_t*)malloc(zcap);
  size_t zn = 0;
  z[zn++] = 0x78; z[zn++] = 0x01;
  size_t off = 0;
  do {
    size_t blk = raw_n - off > 65535 ? 65535 : raw_n - off;
    z[zn++] = (off + blk >= raw_n) ? 1 : 0;
    z[zn++] = (uint8_t)blk; z[zn++] = (uint8_t)(blk >> 8);
    z[zn++] = (uint8_t)~blk; z[zn++] = (uint8_t)(~blk >> 8);
    memcpy(z + zn, raw + off, blk); zn += blk;
    off += blk;
  } while (off < raw_n);
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
  be32(z + zn, (b << 16) | a); zn += 4;

  png_chunk(f, "IDAT", z, zn);
  png_chunk(f, "IEND", NULL, 0);
  fclose(f);
  free(raw); free(z);
  return 0;
}

// ---------------------------------------------------------------------------
// resource ids
// ---------------------------------------------------------------------------
static uint32_t g_rid_base, g_rid_mask, g_root, g_root_cmap, g_root_visual;
static uint32_t g_next_rid;
static uint32_t new_rid(void) { return g_rid_base | (++g_next_rid & g_rid_mask); }

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
static void do_setup(void) {
  section("connection setup");
  uint8_t s[12];
  memset(s, 0, sizeof s);
  s[0] = 'l'; s[1] = 0;
  w16(s + 2, 11); w16(s + 4, 0);
  w16(s + 6, 0);  w16(s + 8, 0);
  wr(s, sizeof s);

  uint8_t pre[8];
  if (!rd_exact(pre, 8, 3000)) { check(0, "setup: no reply from server"); return; }
  check(pre[0] == 1, "setup reply status = %u (1 = Success)", pre[0]);
  check(r16(pre + 2) == 11 && r16(pre + 4) == 0, "protocol version %u.%u", r16(pre + 2), r16(pre + 4));

  uint32_t words = r16(pre + 6);
  size_t extra = (size_t)words * 4;
  uint8_t* b = (uint8_t*)malloc(extra);
  check(rd_exact(b, extra, 3000) == 1,
        "setup reply: read all %zu additional bytes announced by length=%u", extra, words);

  // parse exactly the way libX11/xcb does and confirm we consume every byte
  g_rid_base = r32(b + 4);
  g_rid_mask = r32(b + 8);
  uint16_t vlen  = r16(b + 16);
  uint16_t maxrq = r16(b + 18);
  uint8_t nscreens = b[20], nformats = b[21];
  uint8_t byte_order = b[22], bit_order = b[23];
  uint8_t unit = b[24], pad = b[25], minkc = b[26], maxkc = b[27];

  size_t o = 32;
  char vendor[64]; memset(vendor, 0, sizeof vendor);
  if (vlen < sizeof vendor) memcpy(vendor, b + o, vlen);
  o += (vlen + 3) & ~3u;
  size_t fmt_off = o;
  o += (size_t)nformats * 8;

  int have24 = 0, have32 = 0, have1 = 0;
  for (int i = 0; i < nformats; i++) {
    uint8_t depth = b[fmt_off + i * 8];
    if (depth == 1)  have1 = 1;
    if (depth == 24) have24 = 1;
    if (depth == 32) have32 = 1;
  }

  int found24 = 0, found32 = 0;
  for (int sc = 0; sc < nscreens; sc++) {
    g_root        = r32(b + o + 0);
    g_root_cmap   = r32(b + o + 4);
    g_root_visual = r32(b + o + 32);
    uint16_t sw = r16(b + o + 20), sh = r16(b + o + 22);
    uint8_t root_depth = b[o + 38], ndepths = b[o + 39];
    printf("  info  screen %d: root=0x%x %ux%u depth=%u cmap=0x%x visual=0x%x depths=%u\n",
           sc, g_root, sw, sh, root_depth, g_root_cmap, g_root_visual, ndepths);
    o += 40;
    for (int di = 0; di < ndepths; di++) {
      uint8_t depth = b[o];
      uint16_t nvis = r16(b + o + 2);
      o += 8;
      for (int vi = 0; vi < nvis; vi++) {
        uint32_t vid = r32(b + o);
        uint8_t cls = b[o + 4];
        uint32_t rm = r32(b + o + 8), gm = r32(b + o + 12), bm = r32(b + o + 16);
        if (vid == 0x21 && depth == 24 && cls == 4 &&
            rm == 0x00FF0000 && gm == 0x0000FF00 && bm == 0x000000FF) found24 = 1;
        if (vid == 0x22 && depth == 32 && cls == 4) found32 = 1;
        o += 24;
      }
    }
  }

  check(o == extra,
        "setup reply length field is EXACT: parsed %zu bytes, length announced %zu", o, extra);
  check(!strcmp(vendor, "rdrift"), "vendor string = \"%s\"", vendor);
  check(nscreens == 1, "screen count = %u", nscreens);
  check(have1 && have24 && have32, "pixmap formats include depths 1/24/32 (n=%u)", nformats);
  check(found24, "24-bit TrueColor visual 0x21 present with RGB 8:8:8 masks");
  check(found32, "32-bit TrueColor visual 0x22 present");
  check(byte_order == 0 && bit_order == 0, "image byte order / bitmap bit order = LSBFirst");
  check(unit == 32 && pad == 32, "scanline unit/pad = %u/%u", unit, pad);
  check(minkc >= 8 && maxkc >= minkc, "keycode range %u..%u", minkc, maxkc);
  check(g_rid_mask != 0 && maxrq >= 4096, "rid-base=0x%x mask=0x%x max-request=%u",
        g_rid_base, g_rid_mask, maxrq);
  free(b);
}

// ---------------------------------------------------------------------------
// helper request builders
// ---------------------------------------------------------------------------
static uint32_t create_window(uint32_t parent, int x, int y, int w, int h,
                              uint32_t bg, uint32_t evmask) {
  uint32_t wid = new_rid();
  uint8_t d[36];
  memset(d, 0, sizeof d);
  w32(d + 0, wid); w32(d + 4, parent);
  w16(d + 8, (uint16_t)x); w16(d + 10, (uint16_t)y);
  w16(d + 12, (uint16_t)w); w16(d + 14, (uint16_t)h);
  w16(d + 16, 0);                                     // border width
  w16(d + 18, 1);                                     // class InputOutput
  w32(d + 20, g_root_visual);
  w32(d + 24, 0x00000802);                            // CWBackPixel | CWEventMask
  w32(d + 28, bg);
  w32(d + 32, evmask);
  req(1, 24, d, sizeof d);
  return wid;
}
static uint32_t create_gc(uint32_t drawable, uint32_t fg, uint32_t bg) {
  uint32_t gc = new_rid();
  uint8_t d[20];
  w32(d + 0, gc); w32(d + 4, drawable);
  w32(d + 8, 0x0000000C);                             // GCForeground | GCBackground
  w32(d + 12, fg); w32(d + 16, bg);
  req(55, 0, d, sizeof d);
  return gc;
}
static void change_gc_fg(uint32_t gc, uint32_t fg) {
  uint8_t d[12];
  w32(d + 0, gc); w32(d + 4, 0x00000004); w32(d + 8, fg);
  req(56, 0, d, sizeof d);
}
static void fill_arc(uint32_t drw, uint32_t gc, int x, int y, int w, int h, int a1, int a2) {
  uint8_t d[20];
  w32(d + 0, drw); w32(d + 4, gc);
  w16(d + 8, (uint16_t)x); w16(d + 10, (uint16_t)y);
  w16(d + 12, (uint16_t)w); w16(d + 14, (uint16_t)h);
  w16(d + 16, (uint16_t)a1); w16(d + 18, (uint16_t)a2);
  req(71, 0, d, sizeof d);
}
static void fill_rect_req(uint32_t drw, uint32_t gc, int x, int y, int w, int h) {
  uint8_t d[16];
  w32(d + 0, drw); w32(d + 4, gc);
  w16(d + 8, (uint16_t)x); w16(d + 10, (uint16_t)y);
  w16(d + 12, (uint16_t)w); w16(d + 14, (uint16_t)h);
  req(70, 0, d, sizeof d);
}

// GetImage; returns malloc'd pixel array (0x00RRGGBB) of w*h, or NULL
static uint32_t* get_image(uint32_t drw, int x, int y, int w, int h) {
  uint8_t d[16];
  w32(d + 0, drw);
  w16(d + 4, (uint16_t)x); w16(d + 6, (uint16_t)y);
  w16(d + 8, (uint16_t)w); w16(d + 10, (uint16_t)h);
  w32(d + 12, 0xFFFFFFFFu);
  uint32_t seq = req(73, 2 /*ZPixmap*/, d, sizeof d);
  uint8_t hdr[32]; uint8_t* tail = NULL; size_t tlen = 0;
  if (!expect_reply(seq, hdr, &tail, &tlen, "GetImage")) { free(tail); return NULL; }
  size_t stride = ((size_t)w * 4 + 3) & ~(size_t)3;
  if (tlen < stride * (size_t)h) {
    check(0, "GetImage: short data %zu < %zu", tlen, stride * (size_t)h);
    free(tail); return NULL;
  }
  uint32_t* px = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)w * h);
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < w; xx++)
      px[(size_t)yy * w + xx] = r32(tail + (size_t)yy * stride + (size_t)xx * 4) & 0x00FFFFFFu;
  free(tail);
  return px;
}

static uint32_t cap_px(int x, int y) {
  uint32_t v = 0xFF00FF;
  pthread_mutex_lock(&g_cap_lock);
  if (g_cap && x >= 0 && y >= 0 && x < (int)g_cap_w && y < (int)g_cap_h) {
    const uint8_t* p = g_cap + ((size_t)y * g_cap_w + x) * 4;
    v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
  }
  pthread_mutex_unlock(&g_cap_lock);
  return v;
}

// ---------------------------------------------------------------------------
int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  mkdir("/Users/dperry/linux-shim/tests", 0755);
  unlink(SOCK_PATH);

  if (x11srv_start(SOCK_PATH) != 0) { fprintf(stderr, "x11srv_start failed\n"); return 2; }

  g_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof sa.sun_path, "%s", SOCK_PATH);
  for (int i = 0; i < 100; i++) {
    if (connect(g_fd, (struct sockaddr*)&sa, sizeof sa) == 0) break;
    struct timespec ts = { 0, 10 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    if (i == 99) { fprintf(stderr, "connect failed: %s\n", strerror(errno)); return 2; }
  }

  do_setup();

  uint8_t hdr[32]; uint8_t* tail; size_t tlen;

  // ---- extensions ---------------------------------------------------------
  section("QueryExtension (98)");
  {
    struct { const char* name; int present; uint8_t major; } want[3] = {
      { "BIG-REQUESTS", 1, 133 }, { "GLX", 1, 149 }, { "RANDR", 0, 0 }
    };
    for (int i = 0; i < 3; i++) {
      size_t n = strlen(want[i].name);
      size_t dl = 4 + ((n + 3) & ~(size_t)3);
      uint8_t* d = (uint8_t*)calloc(1, dl);
      w16(d, (uint16_t)n);
      memcpy(d + 4, want[i].name, n);
      uint32_t seq = req(98, 0, d, dl);
      free(d);
      if (!expect_reply(seq, hdr, &tail, &tlen, "QueryExtension")) continue;
      check(hdr[8] == want[i].present && (!want[i].present || hdr[9] == want[i].major),
            "QueryExtension(\"%s\") present=%u major=%u (want present=%d major=%u)",
            want[i].name, hdr[8], hdr[9], want[i].present, want[i].major);
      free(tail);
    }
  }

  section("BIG-REQUESTS enable (133/0)");
  {
    uint32_t seq = req(133, 0, NULL, 0);
    if (expect_reply(seq, hdr, &tail, &tlen, "BigReqEnable")) {
      check(r32(hdr + 8) >= 65535, "BigReqEnable -> maximum-request-length %u units", r32(hdr + 8));
      free(tail);
    }
  }

  // ---- windows ------------------------------------------------------------
  section("windows: CreateWindow(1) MapWindow(8) MapSubwindows(9)");
  const uint32_t EVMASK = 0x00008000u | 0x00020000u | 0x00080000u;   // Expose|Structure|Substructure
  uint32_t win   = create_window(g_root, 100, 60, 420, 260, 0x00FFFFFF, EVMASK);
  uint32_t child = create_window(win, 20, 20, 360, 200, 0x00E0E0E8, EVMASK);
  uint32_t deep  = create_window(child, 250, 130, 90, 50, 0x00303040, EVMASK);

  g_nev = 0;
  req(8, 0, &win, 4);                                   // MapWindow(win)
  drain(120);
  check(count_events(19, win) >= 1, "MapWindow -> MapNotify for 0x%x", win);
  check(count_events(12, win) >= 1, "MapWindow -> Expose for 0x%x", win);

  g_nev = 0;
  req(9, 0, &win, 4);                                   // MapSubwindows(win)
  req(9, 0, &child, 4);                                 // MapSubwindows(child)
  drain(150);
  check(count_events(19, child) >= 1, "MapSubwindows -> MapNotify for child 0x%x", child);
  check(count_events(12, child) >= 1, "MapSubwindows -> Expose for child 0x%x", child);
  check(count_events(19, deep) >= 1, "MapSubwindows -> MapNotify for grandchild 0x%x", deep);
  check(count_events(12, deep) >= 1, "MapSubwindows -> Expose for grandchild 0x%x", deep);

  section("GetGeometry(14) / QueryTree(15) / GetWindowAttributes(3) / GetInputFocus(43)");
  {
    uint32_t seq = req(14, 0, &child, 4);
    if (expect_reply(seq, hdr, &tail, &tlen, "GetGeometry")) {
      check(r16(hdr + 12) == 20 && r16(hdr + 14) == 20 && r16(hdr + 16) == 360 && r16(hdr + 18) == 200,
            "GetGeometry(child) = %ux%u+%d+%d depth=%u",
            r16(hdr + 16), r16(hdr + 18), (int16_t)r16(hdr + 12), (int16_t)r16(hdr + 14), hdr[1]);
      free(tail);
    }
    seq = req(15, 0, &win, 4);
    if (expect_reply(seq, hdr, &tail, &tlen, "QueryTree")) {
      uint16_t nkids = r16(hdr + 16);
      check(nkids == 1 && tlen >= 4 && r32(tail) == child,
            "QueryTree(win) -> %u child, first = 0x%x", nkids, tlen >= 4 ? r32(tail) : 0);
      free(tail);
    }
    seq = req(3, 0, &win, 4);
    if (expect_reply(seq, hdr, &tail, &tlen, "GetWindowAttributes")) {
      check(tlen == 12 && hdr[26] == 2 && r32(tail + 4) == EVMASK,
            "GetWindowAttributes: map-state=%u your-event-mask=0x%x (reply 44 bytes)",
            hdr[26], tlen >= 8 ? r32(tail + 4) : 0);
      free(tail);
    }
    seq = req(43, 0, NULL, 0);
    if (expect_reply(seq, hdr, &tail, &tlen, "GetInputFocus")) {
      check(r32(hdr + 8) != 0, "GetInputFocus -> 0x%x", r32(hdr + 8));
      free(tail);
    }
  }

  // ---- MANDATORY: QueryColors + QueryPointer ------------------------------
  section("QueryColors(91) — libXt allocates colors before it makes a window");
  {
    const uint32_t px[4] = { 0x00FF8000, 0x00123456, 0x00FFFFFF, 0x00000000 };
    uint8_t d[4 + 16];
    w32(d, g_root_cmap);
    for (int i = 0; i < 4; i++) w32(d + 4 + i * 4, px[i]);
    uint32_t seq = req(91, 0, d, sizeof d);
    if (expect_reply(seq, hdr, &tail, &tlen, "QueryColors")) {
      uint16_t n = r16(hdr + 8);
      check(n == 4, "QueryColors -> numRGBs = %u (want 4)", n);
      check(tlen == 32 && r32(hdr + 4) == 8,
            "QueryColors reply length = %u words = %zu bytes for 4 colours", r32(hdr + 4), tlen);
      int ok = 1;
      for (int i = 0; i < 4 && tlen >= 32; i++) {
        uint16_t rr = r16(tail + i * 8), gg = r16(tail + i * 8 + 2), bb = r16(tail + i * 8 + 4);
        if (rr != ((px[i] >> 16) & 255) * 257 || gg != ((px[i] >> 8) & 255) * 257 ||
            bb != (px[i] & 255) * 257) ok = 0;
      }
      check(ok, "QueryColors expands each 8-bit component to 16-bit (comp*257)");
      free(tail);
    }
  }

  section("QueryPointer(38) — xeyes polls this every frame with the LIVE cursor");
  // x11srv_pointer() is local to the Metal window == the presented top-level, so
  // root coords are (top-level origin + cursor).  win is at +100+60, child at +20+20.
  const int ROOT_CX = 100 + CURSOR_X, ROOT_CY = 60 + CURSOR_Y;
  {
    uint32_t seq = req(38, 0, &g_root, 4);
    if (expect_reply(seq, hdr, &tail, &tlen, "QueryPointer")) {
      check(hdr[1] == 1, "QueryPointer same-screen = %u", hdr[1]);
      check(r32(hdr + 8) == g_root, "QueryPointer root = 0x%x", r32(hdr + 8));
      check((int16_t)r16(hdr + 16) == ROOT_CX && (int16_t)r16(hdr + 18) == ROOT_CY,
            "QueryPointer root-x/root-y = %d,%d (live cursor %d,%d in the presented window)",
            (int16_t)r16(hdr + 16), (int16_t)r16(hdr + 18), CURSOR_X, CURSOR_Y);
      free(tail);
    }
    seq = req(38, 0, &win, 4);
    if (expect_reply(seq, hdr, &tail, &tlen, "QueryPointer(top-level)")) {
      check((int16_t)r16(hdr + 20) == CURSOR_X && (int16_t)r16(hdr + 22) == CURSOR_Y,
            "QueryPointer(top-level) win-x/win-y = %d,%d == the raw live cursor",
            (int16_t)r16(hdr + 20), (int16_t)r16(hdr + 22));
      free(tail);
    }
    seq = req(38, 0, &child, 4);
    if (expect_reply(seq, hdr, &tail, &tlen, "QueryPointer(child)")) {
      check((int16_t)r16(hdr + 20) == CURSOR_X - 20 && (int16_t)r16(hdr + 22) == CURSOR_Y - 20,
            "QueryPointer(child at +20+20) win-x/win-y = %d,%d (want %d,%d)",
            (int16_t)r16(hdr + 20), (int16_t)r16(hdr + 22), CURSOR_X - 20, CURSOR_Y - 20);
      free(tail);
    }
  }

  // ---- atoms / properties -------------------------------------------------
  section("InternAtom(16) / GetAtomName(17) / ChangeProperty(18) / GetProperty(20)");
  uint32_t a_wmname = 0, a_custom = 0, a_string = 0;
  {
    const char* names[3] = { "WM_NAME", "_RDRIFT_TEST", "STRING" };
    uint32_t* out[3] = { &a_wmname, &a_custom, &a_string };
    for (int i = 0; i < 3; i++) {
      size_t n = strlen(names[i]);
      size_t dl = 4 + ((n + 3) & ~(size_t)3);
      uint8_t* d = (uint8_t*)calloc(1, dl);
      w16(d, (uint16_t)n);
      memcpy(d + 4, names[i], n);
      uint32_t seq = req(16, 0, d, dl);
      free(d);
      if (expect_reply(seq, hdr, &tail, &tlen, "InternAtom")) { *out[i] = r32(hdr + 8); free(tail); }
    }
    check(a_wmname == 39 && a_string == 31,
          "InternAtom finds predefined atoms: WM_NAME=%u STRING=%u", a_wmname, a_string);
    check(a_custom > 68, "InternAtom allocates a new atom for _RDRIFT_TEST -> %u", a_custom);

    uint32_t seq = req(17, 0, &a_custom, 4);
    if (expect_reply(seq, hdr, &tail, &tlen, "GetAtomName")) {
      uint16_t n = r16(hdr + 8);
      check(n == 12 && tlen >= 12 && !memcmp(tail, "_RDRIFT_TEST", 12),
            "GetAtomName(%u) -> \"%.*s\"", a_custom, (int)n, (const char*)tail);
      free(tail);
    }

    const char* val = "linux-shim x11srv";
    size_t vn = strlen(val);
    size_t dl = 20 + ((vn + 3) & ~(size_t)3);
    uint8_t* d = (uint8_t*)calloc(1, dl);
    w32(d + 0, win); w32(d + 4, a_wmname); w32(d + 8, a_string);
    d[12] = 8;
    w32(d + 16, (uint32_t)vn);
    memcpy(d + 20, val, vn);
    req(18, 0, d, dl);
    free(d);

    uint8_t g[20];
    w32(g + 0, win); w32(g + 4, a_wmname); w32(g + 8, 0);
    w32(g + 12, 0); w32(g + 16, 1024);
    seq = req(20, 0, g, sizeof g);
    if (expect_reply(seq, hdr, &tail, &tlen, "GetProperty")) {
      check(hdr[1] == 8 && r32(hdr + 8) == a_string && r32(hdr + 16) == vn &&
            tlen >= vn && !memcmp(tail, val, vn),
            "GetProperty(WM_NAME) -> format=%u type=%u len=%u \"%.*s\"",
            hdr[1], r32(hdr + 8), r32(hdr + 16), (int)vn, (const char*)tail);
      free(tail);
    }
  }

  // ---- drawing ------------------------------------------------------------
  section("rasterizer: PolyFillArc(71) and friends");
  uint32_t gc_black = create_gc(win, 0x00000000, 0x00FFFFFF);
  uint32_t gc_white = create_gc(win, 0x00FFFFFF, 0x00FFFFFF);
  uint32_t gc_red   = create_gc(child, 0x00FF2020, 0x00E0E0E8);
  uint32_t gc_blue  = create_gc(child, 0x002040FF, 0x00E0E0E8);

  // parent: a black filled ellipse in the lower-left corner (visible outside the child)
  fill_arc(win, gc_black, 4, 226, 30, 30, 0, 360 * 64);

  // child: an xeyes-style pair of eyes tracking the (stubbed) cursor
  int pupil_x[2] = { 0, 0 }, pupil_y[2] = { 0, 0 };
  {
    const int ex[2] = { 90, 250 }, ey = 90, erx = 70, ery = 80;
    for (int i = 0; i < 2; i++) {
      fill_arc(child, gc_black, ex[i] - erx, ey - ery, erx * 2, ery * 2, 0, 360 * 64);
      change_gc_fg(gc_black, 0x00FFFFFF);
      fill_arc(child, gc_black, ex[i] - erx + 6, ey - ery + 6, (erx - 6) * 2, (ery - 6) * 2, 0, 360 * 64);
      change_gc_fg(gc_black, 0x00000000);
      // pupil, offset toward the cursor in child coordinates
      double cx = CURSOR_X - 20, cy = CURSOR_Y - 20;
      double dx = cx - ex[i], dy = cy - ey;
      double len = sqrt(dx * dx + dy * dy);
      double s = len > 1 ? 34.0 / len : 0;
      if (s > 1) s = 1;
      int px_ = ex[i] + (int)(dx * s), py_ = ey + (int)(dy * s);
      pupil_x[i] = px_; pupil_y[i] = py_;
      fill_arc(child, gc_black, px_ - 18, py_ - 18, 36, 36, 0, 360 * 64);
    }
  }

  // exercise the rest of the raster ops in the grandchild-free lower band of the child
  {
    uint8_t d[8 + 8 * 3];
    w32(d + 0, child); w32(d + 4, gc_red);
    // PolySegment(66): three segments
    w16(d + 8, 10);  w16(d + 10, 180); w16(d + 12, 120); w16(d + 14, 180);
    w16(d + 16, 10); w16(d + 18, 186); w16(d + 20, 120); w16(d + 22, 186);
    w16(d + 24, 10); w16(d + 26, 192); w16(d + 28, 120); w16(d + 30, 192);
    req(66, 0, d, sizeof d);
  }
  {
    uint8_t d[8 + 8];                                   // PolyRectangle(67)
    w32(d + 0, child); w32(d + 4, gc_blue);
    w16(d + 8, 130); w16(d + 10, 172); w16(d + 12, 60); w16(d + 14, 24);
    req(67, 0, d, sizeof d);
  }
  {
    uint8_t d[8 + 4 * 3];                               // PolyLine(65), CoordModeOrigin
    w32(d + 0, child); w32(d + 4, gc_blue);
    w16(d + 8, 200);  w16(d + 10, 195);
    w16(d + 12, 215); w16(d + 14, 170);
    w16(d + 16, 230); w16(d + 18, 195);
    req(65, 0, d, sizeof d);
  }
  {
    uint8_t d[12 + 4 * 3];                              // FillPoly(69), Complex/Origin
    w32(d + 0, child); w32(d + 4, gc_red);
    d[8] = 0; d[9] = 0;
    w16(d + 12, 245); w16(d + 14, 195);
    w16(d + 16, 262); w16(d + 18, 168);
    w16(d + 20, 279); w16(d + 22, 195);
    req(69, 0, d, sizeof d);
  }
  {
    uint8_t d[8 + 12];                                  // PolyArc(68), a stroked ellipse
    w32(d + 0, child); w32(d + 4, gc_blue);
    w16(d + 8, 292); w16(d + 10, 166); w16(d + 12, 46); w16(d + 14, 32);
    w16(d + 16, 0);  w16(d + 18, (uint16_t)(360 * 64));
    req(68, 0, d, sizeof d);
  }
  {
    uint8_t d[8 + 4 * 4];                               // PolyPoint(64)
    w32(d + 0, child); w32(d + 4, gc_black);
    for (int i = 0; i < 4; i++) { w16(d + 8 + i * 4, (uint16_t)(340 + i * 3)); w16(d + 10 + i * 4, 190); }
    req(64, 0, d, sizeof d);
  }
  // ImageText8(76) into the parent's visible bottom strip
  {
    const char* s = "x11srv PolyFillArc";
    size_t n = strlen(s);
    size_t dl = 12 + ((n + 3) & ~(size_t)3);
    uint8_t* d = (uint8_t*)calloc(1, dl);
    w32(d + 0, win); w32(d + 4, gc_black);
    w16(d + 8, 40); w16(d + 10, 246);
    memcpy(d + 12, s, n);
    req(76, (uint8_t)n, d, dl);
    free(d);
  }
  // CreatePixmap(53) + PutImage(72) + CopyArea(62) + FreePixmap(54)
  {
    uint32_t pix = new_rid();
    uint8_t cp[12];
    w32(cp + 0, pix); w32(cp + 4, win);
    w16(cp + 8, 16); w16(cp + 10, 16);
    req(53, 24, cp, sizeof cp);

    uint8_t img[20 + 16 * 16 * 4];
    memset(img, 0, sizeof img);
    w32(img + 0, pix); w32(img + 4, gc_black);
    w16(img + 8, 16); w16(img + 10, 16);
    w16(img + 12, 0); w16(img + 14, 0);
    img[16] = 0; img[17] = 24;
    for (int y = 0; y < 16; y++)
      for (int x = 0; x < 16; x++)
        w32(img + 20 + (y * 16 + x) * 4, ((x ^ y) & 1) ? 0x0000C000 : 0x00FFD000);
    req(72, 2 /*ZPixmap*/, img, sizeof img);

    uint8_t ca[24];
    w32(ca + 0, pix); w32(ca + 4, win); w32(ca + 8, gc_black);
    w16(ca + 12, 0); w16(ca + 14, 0);
    w16(ca + 16, 392); w16(ca + 18, 232);
    w16(ca + 20, 16); w16(ca + 22, 16);
    req(62, 0, ca, sizeof ca);

    req(54, 0, &pix, 4);
  }
  // ClearArea(61) on the grandchild, then repaint it
  {
    uint8_t d[12];
    w32(d + 0, deep);
    w16(d + 4, 0); w16(d + 6, 0); w16(d + 8, 0); w16(d + 10, 0);
    req(61, 1 /*exposures*/, d, sizeof d);
    uint32_t gcd = create_gc(deep, 0x0000FF80, 0x00303040);
    fill_rect_req(deep, gcd, 8, 8, 74, 34);
  }

  drain(60);

  section("GetImage(73) — the raster actually changed the framebuffer");
  {
    uint32_t* p = get_image(win, 0, 216, 50, 44);
    if (p) {
      uint32_t centre = p[(240 - 216) * 50 + 19];        // inside the black ellipse
      uint32_t corner = p[(220 - 216) * 50 + 45];        // outside it
      check(centre == 0x000000, "PolyFillArc: pixel inside the ellipse = 0x%06x (black)", centre);
      check(corner == 0xFFFFFF, "PolyFillArc: pixel outside the ellipse = 0x%06x (white bg)", corner);
      free(p);
    }
    // the eye is three nested filled arcs: black rim, white sclera, black pupil
    p = get_image(child, 90, 11, 1, 1);
    if (p) { check(p[0] == 0x000000, "eye rim (top of the outer arc) = 0x%06x", p[0]); free(p); }
    p = get_image(child, 60, 60, 1, 1);
    if (p) { check(p[0] == 0xFFFFFF, "eye sclera (inner white arc) = 0x%06x", p[0]); free(p); }
    p = get_image(child, pupil_x[0], pupil_y[0], 1, 1);
    if (p) {
      check(p[0] == 0x000000, "eye PUPIL at (%d,%d) = 0x%06x — tracks the live cursor",
            pupil_x[0], pupil_y[0], p[0]);
      free(p);
    }
    p = get_image(win, 30, 30, 2, 2);
    if (p) {
      check(p[0] == 0xFFFFFF,
            "parent fb under the child is still its own bg 0x%06x (children are separate drawables)",
            p[0]);
      free(p);
    }
    p = get_image(win, 392, 232, 4, 4);
    if (p) {
      check(p[0] == 0xFFD000 && p[1] == 0x00C000,
            "PutImage+CopyArea checkerboard landed: 0x%06x 0x%06x", p[0], p[1]);
      free(p);
    }
  }

  // ---- presentation -------------------------------------------------------
  section("present: coalesced mirror + composite_children");
  {
    for (int i = 0; i < 60 && g_cap_n == 0; i++) {
      struct timespec ts = { 0, 20 * 1000 * 1000 };
      nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(&g_cap_lock);
    int frames = g_cap_n, opens = g_open_n;
    uint32_t cw = g_cap_w, ch = g_cap_h, ow = g_open_w, oh = g_open_h;
    pthread_mutex_unlock(&g_cap_lock);
    check(frames > 0, "x11rnd_present called %d time(s) after the client went quiet", frames);
    check(opens > 0 && ow == 420 && oh == 260, "x11rnd_open(%u,%u)", ow, oh);
    check(cw == 420 && ch == 260, "presented frame is %ux%u (the top-level window)", cw, ch);
    check(cap_px(2, 2) == 0xFFFFFF, "presented: parent bg outside the child = 0x%06x", cap_px(2, 2));
    check(cap_px(30, 30) == 0xE0E0E8,
          "presented: CHILD window composited at +20+20 = 0x%06x (parent fb there is white)",
          cap_px(30, 30));
    check(cap_px(20 + pupil_x[0], 20 + pupil_y[0]) == 0x000000,
          "presented: child's eye PUPIL at (%d,%d) = 0x%06x",
          20 + pupil_x[0], 20 + pupil_y[0], cap_px(20 + pupil_x[0], 20 + pupil_y[0]));
    check(cap_px(20 + 250 + 85, 20 + 130 + 45) == 0x303040,
          "presented: GRANDCHILD bg composited recursively = 0x%06x", cap_px(355, 195));
    check(cap_px(20 + 250 + 40, 20 + 130 + 25) == 0x00FF80,
          "presented: grandchild's own drawing = 0x%06x", cap_px(310, 175));

    pthread_mutex_lock(&g_cap_lock);
    if (g_cap) png_write(PNG_PATH, g_cap, g_cap_w, g_cap_h);
    pthread_mutex_unlock(&g_cap_lock);
    printf("  info  framebuffer dumped to %s\n", PNG_PATH);
  }

  // ---- GLX ----------------------------------------------------------------
  section("GLX (major 149)");
  {
    uint8_t d[32];
    // 7 QueryVersion
    w32(d + 0, 1); w32(d + 4, 4);
    uint32_t seq = req(149, 7, d, 8);
    if (expect_reply(seq, hdr, &tail, &tlen, "GLX QueryVersion")) {
      check(r32(hdr + 8) == 1 && r32(hdr + 12) == 4,
            "GLX QueryVersion -> %u.%u", r32(hdr + 8), r32(hdr + 12));
      free(tail);
    }

    // 14 GetVisualConfigs
    w32(d + 0, 0);
    seq = req(149, 14, d, 4);
    if (expect_reply(seq, hdr, &tail, &tlen, "GLX GetVisualConfigs")) {
      uint32_t len = r32(hdr + 4), nvis = r32(hdr + 8), nprops = r32(hdr + 12);
      check(nvis == 1 && nprops == 18, "GetVisualConfigs -> %u visual(s), %u props", nvis, nprops);
      check(len == nvis * nprops && tlen == (size_t)len * 4,
            "GetVisualConfigs length %u words == numVisuals*numProps (%u); read %zu bytes",
            len, nvis * nprops, tlen);
      if (tlen >= 8)
        check(r32(tail) == 0x21 && r32(tail + 4) == 4,
              "GetVisualConfigs prop[0]=visualID 0x%x prop[1]=class %u (TrueColor)",
              r32(tail), r32(tail + 4));
      free(tail);
    }

    // 21 GetFBConfigs — THE length bug
    w32(d + 0, 0);
    seq = req(149, 21, d, 4);
    if (expect_reply(seq, hdr, &tail, &tlen, "GLX GetFBConfigs")) {
      uint32_t len = r32(hdr + 4), ncfg = r32(hdr + 8), npairs = r32(hdr + 12);
      // what an xcb client computes for the property list, in CARD32s:
      uint32_t xcb_words = ncfg * npairs * 2;
      check(ncfg == 1 && npairs > 10,
            "GetFBConfigs -> %u fbconfig(s), numAttribs = %u PAIRS", ncfg, npairs);
      check(len == xcb_words,
            "GetFBConfigs reply length %u words == numFBConfigs*numAttribs*2 = %u  "
            "(the xcb 'Too much data' bug)", len, xcb_words);
      check(tlen == (size_t)xcb_words * 4,
            "GetFBConfigs: bytes actually sent %zu == what a client reads %u", tlen, xcb_words * 4);
      int found_id = 0, found_vis = 0, found_dbl = 0;
      for (uint32_t i = 0; i + 1 < xcb_words && (size_t)(i + 2) * 4 <= tlen; i += 2) {
        uint32_t a = r32(tail + i * 4), v = r32(tail + i * 4 + 4);
        if (a == 0x8013) found_id = 1;                  // GLX_FBCONFIG_ID
        if (a == 0x800B && v == 0x21) found_vis = 1;    // GLX_VISUAL_ID
        if (a == 5 && v == 1) found_dbl = 1;            // GLX_DOUBLEBUFFER
      }
      check(found_id && found_vis && found_dbl,
            "GetFBConfigs pairs include FBCONFIG_ID, VISUAL_ID=0x21, DOUBLEBUFFER=1");
      free(tail);
    }

    // 19 QueryServerString
    const char* wantstr[3] = { "rdrift", "1.4", "" };
    for (uint32_t nm = 1; nm <= 3; nm++) {
      w32(d + 0, 0); w32(d + 4, nm);
      seq = req(149, 19, d, 8);
      if (!expect_reply(seq, hdr, &tail, &tlen, "GLX QueryServerString")) continue;
      uint32_t n = r32(hdr + 12);
      check(n == strlen(wantstr[nm - 1]) + 1 && tlen >= n &&
            !strcmp((const char*)tail, wantstr[nm - 1]),
            "GLX QueryServerString(%u) n=%u -> \"%s\"", nm, n, tail ? (const char*)tail : "");
      free(tail);
    }

    // 20 ClientInfo (no reply)
    memset(d, 0, sizeof d);
    w32(d + 0, 1); w32(d + 4, 4); w32(d + 8, 4);
    req(149, 20, d, 16);

    // 3 CreateContext (no reply) then 6 IsDirect, 5 MakeCurrent
    uint32_t ctx = new_rid();
    w32(d + 0, ctx); w32(d + 4, 0x21); w32(d + 8, 0); w32(d + 12, 0);
    d[16] = 0; d[17] = d[18] = d[19] = 0;
    req(149, 3, d, 20);

    w32(d + 0, ctx);
    seq = req(149, 6, d, 4);
    if (expect_reply(seq, hdr, &tail, &tlen, "GLX IsDirect")) {
      check(hdr[8] == 0, "GLX IsDirect -> %u (False = indirect)", hdr[8]);
      free(tail);
    }

    w32(d + 0, win); w32(d + 4, ctx); w32(d + 8, 0);
    seq = req(149, 5, d, 12);
    uint32_t tag = 0;
    if (expect_reply(seq, hdr, &tail, &tlen, "GLX MakeCurrent")) {
      tag = r32(hdr + 8);
      check(tag != 0, "GLX MakeCurrent -> context tag %u (nonzero)", tag);
      free(tail);
    }

    // 129 glGetString
    const uint32_t names[3] = { 0x1F00, 0x1F01, 0x1F02 };
    for (int i = 0; i < 3; i++) {
      w32(d + 0, tag); w32(d + 4, names[i]);
      seq = req(149, 129, d, 8);
      if (!expect_reply(seq, hdr, &tail, &tlen, "GLX glGetString")) continue;
      uint32_t size = r32(hdr + 12);
      check(size > 1 && tlen >= size && ((const char*)tail)[size - 1] == 0,
            "GLX glGetString(0x%x) size=%u -> \"%s\"", names[i], size, (const char*)tail);
      free(tail);
    }

    // 1 GLXRender — a batched GL command stream; must not stall
    {
      uint8_t rq[4 + 8 + 8 + 8];
      w32(rq + 0, tag);
      w16(rq + 4, 8);   w16(rq + 6, 137);               // len=8 op=137 (glClearColor-ish)
      w32(rq + 8, 0);
      w16(rq + 12, 8);  w16(rq + 14, 127);              // glClear
      w32(rq + 16, 0x4000);
      w16(rq + 20, 8);  w16(rq + 22, 174);              // glLoadIdentity-ish
      w32(rq + 24, 0);
      req(149, 1, rq, sizeof rq);
    }
    // a reply-bearing request right after Render proves the stream did not stall
    w32(d + 0, 1); w32(d + 4, 4);
    seq = req(149, 7, d, 8);
    if (expect_reply(seq, hdr, &tail, &tlen, "GLX QueryVersion after Render")) {
      check(r32(hdr + 8) == 1, "GLXRender consumed without stalling the connection");
      free(tail);
    }
  }

  // ---- misc requests that Xt/Xaw fire during startup ----------------------
  section("Xt/Xaw startup requests");
  {
    uint32_t seq;
    uint8_t d[16];
    // AllocColor(84)
    w32(d + 0, g_root_cmap);
    w16(d + 4, 0x8080); w16(d + 6, 0x4040); w16(d + 8, 0x2020); w16(d + 10, 0);
    seq = req(84, 0, d, 12);
    if (expect_reply(seq, hdr, &tail, &tlen, "AllocColor")) {
      check(r32(hdr + 16) == 0x00804020, "AllocColor -> pixel 0x%06x", r32(hdr + 16));
      free(tail);
    }
    // CreateColormap(78)
    uint32_t cm = new_rid();
    w32(d + 0, cm); w32(d + 4, win); w32(d + 8, g_root_visual);
    req(78, 0, d, 12);
    // QueryFont(47)
    uint32_t fid = new_rid();
    w32(d + 0, fid);
    seq = req(47, 0, d, 4);
    if (expect_reply(seq, hdr, &tail, &tlen, "QueryFont")) {
      uint32_t m = tlen >= 28 ? r32(tail + 24) : 0;
      check(m > 0 && tlen == 28 + (size_t)m * 12,
            "QueryFont -> %u charinfos, tail %zu bytes (self-consistent)", m, tlen);
      free(tail);
    }
    // GetKeyboardMapping(101)
    d[0] = 8; d[1] = 8; d[2] = 0; d[3] = 0;
    seq = req(101, 0, d, 4);
    if (expect_reply(seq, hdr, &tail, &tlen, "GetKeyboardMapping")) {
      check(hdr[1] >= 1 && tlen == (size_t)hdr[1] * 8 * 4,
            "GetKeyboardMapping -> %u keysyms/keycode, %zu bytes", hdr[1], tlen);
      free(tail);
    }
    // GetModifierMapping(119)
    seq = req(119, 0, NULL, 0);
    if (expect_reply(seq, hdr, &tail, &tlen, "GetModifierMapping")) {
      check(hdr[1] >= 1 && tlen == (size_t)hdr[1] * 8,
            "GetModifierMapping -> %u keycodes/modifier, %zu bytes", hdr[1], tlen);
      free(tail);
    }
    // Bell(104), no reply
    req(104, 0, NULL, 0);

    // BIG-REQUESTS wire form: 16-bit length 0 + CARD32 unit count.
    // A 96x96 ZPixmap PutImage plus a following normal request that must still parse.
    {
      const int IW = 96, IH = 96;
      size_t dl = 20 + (size_t)IW * IH * 4;
      uint8_t* img = (uint8_t*)calloc(1, dl);
      w32(img + 0, win); w32(img + 4, gc_black);
      w16(img + 8, IW); w16(img + 10, IH);
      w16(img + 12, 300); w16(img + 14, 8);
      img[16] = 0; img[17] = 24;
      for (int y = 0; y < IH; y++)
        for (int x = 0; x < IW; x++)
          w32(img + 20 + ((size_t)y * IW + x) * 4, ((x / 12 + y / 12) & 1) ? 0x00204080 : 0x00F0A030);
      req_big(72, 2 /*ZPixmap*/, img, dl);
      free(img);
      drain(40);
      uint32_t* p = get_image(win, 300, 8, 24, 24);
      if (p) {
        check(p[0] == 0xF0A030 && p[(size_t)2 * 24 + 14] == 0x204080,
              "BIG-REQUESTS: 36884-byte PutImage parsed, pixels 0x%06x / 0x%06x",
              p[0], p[(size_t)2 * 24 + 14]);
        free(p);
      }
      seq = req(38, 0, &g_root, 4);
      if (expect_reply(seq, hdr, &tail, &tlen, "QueryPointer after a big request")) {
        check(1, "request stream stayed in sync across the BIG-REQUESTS header");
        free(tail);
      }
    }
    // ConfigureWindow(12) resize
    g_nev = 0;
    uint8_t cfg[12];
    w32(cfg + 0, win);
    w16(cfg + 4, 0x000C);                               // width | height
    w16(cfg + 6, 0);
    w32(cfg + 8, 420);
    uint8_t cfg2[16];
    memcpy(cfg2, cfg, 8);
    w32(cfg2 + 8, 420); w32(cfg2 + 12, 262);
    req(12, 0, cfg2, sizeof cfg2);
    drain(120);
    check(count_events(22, win) >= 1, "ConfigureWindow -> ConfigureNotify for 0x%x", win);
    // put it back so the PNG we already dumped stays representative
    w32(cfg2 + 8, 420); w32(cfg2 + 12, 260);
    req(12, 0, cfg2, sizeof cfg2);
    drain(80);
    // UnmapWindow(10) / DestroyWindow(4)
    req(10, 0, &deep, 4);
    req(4, 0, &deep, 4);
    // FreeGC(60)
    req(60, 0, &gc_white, 4);
    req(60, 0, &gc_red, 4);
    drain(80);
    // still alive?
    seq = req(38, 0, &g_root, 4);
    if (expect_reply(seq, hdr, &tail, &tlen, "QueryPointer (final liveness)")) {
      check((int16_t)r16(hdr + 16) == ROOT_CX, "server still healthy after the full request sweep");
      free(tail);
    }
  }

  drain(50);
  close(g_fd);

#ifdef HAVE_XCB
  section("cross-check with the real libxcb");
  {
    struct timespec ts = { 0, 80 * 1000 * 1000 };
    nanosleep(&ts, NULL);                               // let the accept loop come back around
    int fd2 = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un s2;
    memset(&s2, 0, sizeof s2);
    s2.sun_family = AF_UNIX;
    snprintf(s2.sun_path, sizeof s2.sun_path, "%s", SOCK_PATH);
    if (connect(fd2, (struct sockaddr*)&s2, sizeof s2) != 0) {
      check(0, "xcb: could not reconnect to the server");
    } else {
      // xcb_connect_to_fd writes the setup request and parses the reply itself:
      // a bad length field here is an immediate connection error.
      xcb_connection_t* c = xcb_connect_to_fd(fd2, NULL);
      check(c && !xcb_connection_has_error(c),
            "xcb_connect_to_fd: real xcb accepted our setup reply (err=%d)",
            c ? xcb_connection_has_error(c) : -1);
      if (c && !xcb_connection_has_error(c)) {
        const xcb_setup_t* setup = xcb_get_setup(c);
        char vend[32]; memset(vend, 0, sizeof vend);
        int vl = xcb_setup_vendor_length(setup);
        if (vl < (int)sizeof vend) memcpy(vend, xcb_setup_vendor(setup), vl);
        xcb_screen_iterator_t si = xcb_setup_roots_iterator(setup);
        check(!strcmp(vend, "rdrift") && si.rem == 1,
              "xcb parsed setup: vendor=\"%s\" screens=%d root=0x%x root_visual=0x%x",
              vend, si.rem, si.data->root, si.data->root_visual);

        const xcb_query_extension_reply_t* gx = xcb_get_extension_data(c, &xcb_glx_id);
        check(gx && gx->present && gx->major_opcode == 149,
              "xcb_get_extension_data(GLX): present=%d major=%d",
              gx ? gx->present : -1, gx ? gx->major_opcode : -1);

        // GLX GetFBConfigs — xcb computes the property list length itself
        xcb_generic_error_t* e = NULL;
        xcb_glx_get_fb_configs_reply_t* fb =
            xcb_glx_get_fb_configs_reply(c, xcb_glx_get_fb_configs(c, 0), &e);
        if (!fb) { check(0, "xcb GLX GetFBConfigs failed (error %d)", e ? e->error_code : -1); }
        else {
          int listlen = xcb_glx_get_fb_configs_property_list_length(fb);
          check((uint32_t)listlen == fb->length,
                "xcb GetFBConfigs: property_list_length=%d == reply length=%u "
                "(num_FB_configs=%u num_properties=%u PAIRS)",
                listlen, fb->length, fb->num_FB_configs, fb->num_properties);
          check(!xcb_connection_has_error(c),
                "xcb connection still healthy after GetFBConfigs (no _XRead overrun)");
          free(fb);
        }

        xcb_glx_get_visual_configs_reply_t* vc =
            xcb_glx_get_visual_configs_reply(c, xcb_glx_get_visual_configs(c, 0), &e);
        if (!vc) { check(0, "xcb GLX GetVisualConfigs failed"); }
        else {
          int ll = xcb_glx_get_visual_configs_property_list_length(vc);
          check((uint32_t)ll == vc->length && vc->num_visuals == 1 && vc->num_properties == 18,
                "xcb GetVisualConfigs: list=%d length=%u visuals=%u props=%u",
                ll, vc->length, vc->num_visuals, vc->num_properties);
          free(vc);
        }

        xcb_glx_query_version_reply_t* qv =
            xcb_glx_query_version_reply(c, xcb_glx_query_version(c, 1, 4), &e);
        check(qv && qv->major_version == 1 && qv->minor_version == 4,
              "xcb GLX QueryVersion -> %u.%u", qv ? qv->major_version : 0, qv ? qv->minor_version : 0);
        free(qv);

        xcb_glx_query_server_string_reply_t* ss =
            xcb_glx_query_server_string_reply(c, xcb_glx_query_server_string(c, 0, 1), &e);
        if (ss) {
          check(xcb_glx_query_server_string_string_length(ss) == 7 &&
                !strncmp(xcb_glx_query_server_string_string(ss), "rdrift", 6),
                "xcb GLX QueryServerString(VENDOR) len=%d",
                xcb_glx_query_server_string_string_length(ss));
          free(ss);
        } else check(0, "xcb GLX QueryServerString failed");

        // core: window, arc, image, pointer, colours
        xcb_screen_t* scr = si.data;
        xcb_window_t w2 = xcb_generate_id(c);
        uint32_t vals[2] = { 0x00FFFFFF, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY };
        xcb_create_window(c, 24, w2, scr->root, 0, 0, 200, 150, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
        xcb_map_window(c, w2);
        xcb_gcontext_t g2 = xcb_generate_id(c);
        uint32_t gv[1] = { 0x00000000 };
        xcb_create_gc(c, g2, w2, XCB_GC_FOREGROUND, gv);
        xcb_arc_t arc = { 20, 20, 160, 110, 0, 360 << 6 };
        xcb_poly_fill_arc(c, w2, g2, 1, &arc);
        xcb_flush(c);

        xcb_get_image_reply_t* im = xcb_get_image_reply(
            c, xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, w2, 0, 0, 200, 150, ~0u), &e);
        if (!im) { check(0, "xcb GetImage failed"); }
        else {
          uint8_t* dp = xcb_get_image_data(im);
          int dl2 = xcb_get_image_data_length(im);
          check(dl2 == 200 * 150 * 4, "xcb GetImage returned %d bytes (200*150*4)", dl2);
          uint32_t centre = r32(dp + (75 * 200 + 100) * 4) & 0xFFFFFF;
          uint32_t corner = r32(dp + (2 * 200 + 2) * 4) & 0xFFFFFF;
          check(centre == 0x000000 && corner == 0xFFFFFF,
                "xcb PolyFillArc: centre=0x%06x corner=0x%06x", centre, corner);
          free(im);
        }

        xcb_query_pointer_reply_t* qp =
            xcb_query_pointer_reply(c, xcb_query_pointer(c, w2), &e);
        check(qp && qp->win_x == CURSOR_X && qp->win_y == CURSOR_Y,
              "xcb QueryPointer -> win %d,%d root %d,%d",
              qp ? qp->win_x : -1, qp ? qp->win_y : -1, qp ? qp->root_x : -1, qp ? qp->root_y : -1);
        free(qp);

        uint32_t cpx[3] = { 0x00FF8000, 0x00102030, 0x00FFFFFF };
        xcb_query_colors_reply_t* qc =
            xcb_query_colors_reply(c, xcb_query_colors(c, scr->default_colormap, 3, cpx), &e);
        if (!qc) { check(0, "xcb QueryColors failed"); }
        else {
          int n = xcb_query_colors_colors_length(qc);
          xcb_rgb_t* rgb = xcb_query_colors_colors(qc);
          check(n == 3 && rgb[0].red == 0xFF * 257 && rgb[0].green == 0x80 * 257 &&
                rgb[1].blue == 0x30 * 257,
                "xcb QueryColors: %d colours, [0]=%04x/%04x/%04x",
                n, rgb[0].red, rgb[0].green, rgb[0].blue);
          free(qc);
        }

        check(!xcb_connection_has_error(c), "xcb connection healthy at the end of the sweep");
        xcb_disconnect(c);
      }
    }
  }
#endif

#ifdef HAVE_XLIB
  section("cross-check with the real libX11 (what Xt/Xaw + xeyes use)");
  x11srv_stop();
  {
    // xcb (and therefore Xlib) treats a DISPLAY of "/prefix:N" as the literal
    // unix socket path "/prefix:N", so the server can live outside /tmp.
    const char* disp = "/Users/dperry/linux-shim/tests/x11srv:0";
    unlink(disp);
    if (x11srv_start(disp) != 0) { check(0, "could not restart the server for Xlib"); }
    else {
      Display* dpy = XOpenDisplay(disp);
      check(dpy != NULL, "XOpenDisplay(\"%s\") — Xlib accepted the setup reply", disp);
      if (dpy) {
        int scr = DefaultScreen(dpy);
        check(DisplayWidth(dpy, scr) == 1280 && DefaultDepth(dpy, scr) == 24 &&
              !strcmp(ServerVendor(dpy), "rdrift"),
              "Xlib: %dx%d depth %d vendor \"%s\" root 0x%lx",
              DisplayWidth(dpy, scr), DisplayHeight(dpy, scr), DefaultDepth(dpy, scr),
              ServerVendor(dpy), (unsigned long)RootWindow(dpy, scr));

        int maj = 0, ev = 0, err = 0;
        check(XQueryExtension(dpy, "GLX", &maj, &ev, &err) && maj == 149,
              "Xlib XQueryExtension(GLX) major=%d", maj);

        int minkc = 0, maxkc = 0;
        XDisplayKeycodes(dpy, &minkc, &maxkc);
        int per = 0;
        KeySym* ks = XGetKeyboardMapping(dpy, (KeyCode)minkc, 4, &per);
        check(ks != NULL && per >= 1, "Xlib XGetKeyboardMapping: %d keysyms/keycode", per);
        if (ks) XFree(ks);
        XModifierKeymap* mk = XGetModifierMapping(dpy);
        check(mk != NULL, "Xlib XGetModifierMapping ok");
        if (mk) XFreeModifiermap(mk);

        Window w3 = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, 240, 180, 0,
                                        BlackPixel(dpy, scr), WhitePixel(dpy, scr));
        XSelectInput(dpy, w3, ExposureMask | StructureNotifyMask);
        XMapWindow(dpy, w3);
        // Xlib apps paint on Expose — prove we deliver it.
        XEvent xe; int got_expose = 0, got_map = 0;
        for (int i = 0; i < 20 && !(got_expose && got_map); i++) {
          XFlush(dpy);
          while (XPending(dpy)) {
            XNextEvent(dpy, &xe);
            if (xe.type == Expose) got_expose = 1;
            if (xe.type == MapNotify) got_map = 1;
          }
          struct timespec t = { 0, 20 * 1000 * 1000 };
          nanosleep(&t, NULL);
        }
        check(got_map, "Xlib received MapNotify");
        check(got_expose, "Xlib received Expose (this is what makes an app paint)");

        GC gc = XCreateGC(dpy, w3, 0, NULL);
        XSetForeground(dpy, gc, BlackPixel(dpy, scr));
        XFillArc(dpy, w3, gc, 20, 20, 200, 140, 0, 360 * 64);
        XFlush(dpy);
        XImage* xi = XGetImage(dpy, w3, 0, 0, 240, 180, AllPlanes, ZPixmap);
        check(xi != NULL, "Xlib XGetImage ok");
        if (xi) {
          unsigned long cc = XGetPixel(xi, 120, 90), co = XGetPixel(xi, 2, 2);
          check((cc & 0xFFFFFF) == 0x000000 && (co & 0xFFFFFF) == 0xFFFFFF,
                "Xlib XFillArc: centre=0x%06lx corner=0x%06lx", cc & 0xFFFFFF, co & 0xFFFFFF);
          XDestroyImage(xi);
        }

        Window rr, cr; int rx, ry, wx, wy; unsigned int mask;
        check(XQueryPointer(dpy, w3, &rr, &cr, &rx, &ry, &wx, &wy, &mask) &&
              wx == CURSOR_X && wy == CURSOR_Y,
              "Xlib XQueryPointer -> win %d,%d root %d,%d", wx, wy, rx, ry);

        XColor xc; xc.red = 0xFF00; xc.green = 0x8000; xc.blue = 0x0000; xc.flags = DoRed|DoGreen|DoBlue;
        check(XAllocColor(dpy, DefaultColormap(dpy, scr), &xc), "Xlib XAllocColor -> pixel 0x%06lx", xc.pixel);
        XColor q[2]; q[0].pixel = 0x00FF8000; q[1].pixel = 0x00102030;
        XQueryColors(dpy, DefaultColormap(dpy, scr), q, 2);
        check(q[0].red == 0xFFFF && q[0].green == 0x8080 && q[1].blue == 0x3030,
              "Xlib XQueryColors -> %04x/%04x/%04x", q[0].red, q[0].green, q[0].blue);

        XFontStruct* fs = XLoadQueryFont(dpy, "fixed");
        check(fs != NULL && fs->max_bounds.width > 0,
              "Xlib XLoadQueryFont(\"fixed\") -> width %d ascent %d",
              fs ? fs->max_bounds.width : 0, fs ? fs->ascent : 0);
        if (fs) {
          XSetForeground(dpy, gc, WhitePixel(dpy, scr));
          XDrawImageString(dpy, w3, gc, 60, 100, "Xlib", 4);
          XFreeFont(dpy, fs);
        }

        Atom a = XInternAtom(dpy, "_XLIB_ROUNDTRIP", False);
        char* nm = XGetAtomName(dpy, a);
        check(nm && !strcmp(nm, "_XLIB_ROUNDTRIP"), "Xlib atom roundtrip -> %s", nm ? nm : "(null)");
        if (nm) XFree(nm);
        XStoreName(dpy, w3, "x11srv");
        XSync(dpy, False);
        check(1, "Xlib survived the whole sweep with no fatal I/O error");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
      }
    }
  }
#endif

  x11srv_stop();

  printf("\n=========================================\n");
  printf("  %d passed, %d failed\n", g_pass, g_fail);
  printf("=========================================\n");
  return g_fail ? 1 : 0;
}
