// x11srv.c — in-process X11 server: X protocol -> CPU framebuffer -> Metal window.
//
// A single-client X11 server good enough to run Xt/Xaw apps (xeyes) and to carry
// glxgears all the way through the GLX handshake.  Everything is rasterized into
// per-drawable RGBA8 CPU framebuffers; the first mapped top-level window is
// mirrored to the Metal window through x11rnd_present() (x11srv_render.m).
//
// Hard-won facts baked in here (see RECOVERY.md) — do not "simplify" these:
//   * QueryColors(91)   — libXt allocates colors BEFORE it creates any window.
//   * QueryPointer(38)  — xeyes polls it every frame; must answer with the LIVE cursor.
//   * MapSubwindows(9)  — xeyes' eyes are a CHILD widget window; without mapping +
//                         exposing children it never paints.
//   * PolyFillArc(71)   — the eyeballs and pupils.  71/68/61 must set the dirty flag.
//   * composite_children — toolkit apps draw into child widget windows, so the
//                         presented snapshot must blit them onto the shell.
//   * GLX GetFBConfigs(21) — numAttribs is the PAIR COUNT and reply length is
//                         pairs*2 words.  Getting this wrong => xcb "Too much data
//                         requested from _XRead" abort.
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
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <time.h>

#include "x11srv.h"
#include <sys/time.h>
#include "gpu.h"          // gpuwin_next_event — host input
#include "glxdecode.h"

// ---------------------------------------------------------------------------
// tunables / protocol constants
// ---------------------------------------------------------------------------
#define ROOT_WIN     0x00000064u
#define ROOT_CMAP    0x00000020u
#define VISUAL_24    0x00000021u
#define VISUAL_32    0x00000022u
#define RID_BASE     0x00400000u
#define RID_MASK     0x001FFFFFu
#define SCREEN_W     1280
#define SCREEN_H     800
#define BIGREQ_MAJOR 133
#define GLX_MAJOR    149
#define XI_MAJOR     131                                // XInputExtension
#define XI_FIRST_EV   66
#define XI_FIRST_ERR 129

// SETofEVENT bits we care about
#define EV_EXPOSURE        0x00008000u
#define EV_STRUCTURE       0x00020000u
#define EV_SUBSTRUCTURE    0x00080000u

// event codes
#define XEV_EXPOSE         12
#define XEV_MAPNOTIFY      19
#define XEV_UNMAPNOTIFY    18
#define XEV_CONFIGURENOTIFY 22

// error codes
#define XERR_REQUEST   1
#define XERR_VALUE     2
#define XERR_WINDOW    3
#define XERR_PIXMAP    4
#define XERR_ATOM      5
#define XERR_DRAWABLE  9
#define XERR_GC        13
#define XERR_IDCHOICE  14
#define XERR_IMPLEMENTATION 17

#define MAX_DRAWABLES 512
#define MAX_GCS       256
#define MAX_ATOMS     1024
#define MAX_PROPS     512
#define MAX_CMAPS     64

#define PAD4(n) (((n) + 3u) & ~3u)

// ---------------------------------------------------------------------------
// little-endian accessors (client and host are both LE here)
// ---------------------------------------------------------------------------
static inline uint16_t g16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t g32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int16_t  gs16(const uint8_t* p) { return (int16_t)g16(p); }
static inline void p16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void p32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
static int g_verbose = -1;
static void xlog(const char* fmt, ...) {
  if (g_verbose < 0) { const char* e = getenv("X11SRV_DEBUG"); g_verbose = (e && *e && *e != '0'); }
  if (!g_verbose) return;
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[x11] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
  va_end(ap);
}
static void xwarn(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[x11] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
  va_end(ap);
}

// ---------------------------------------------------------------------------
// server state
// ---------------------------------------------------------------------------
typedef struct {
  uint32_t id;
  int      used;
  int      is_window;
  int32_t  x, y;              // parent-relative origin (windows)
  uint32_t w, h;
  uint16_t border_width;
  uint8_t  depth;
  uint16_t class_;            // 1 InputOutput, 2 InputOnly
  uint32_t parent;
  int      mapped;
  int      override_redirect;
  uint32_t event_mask;
  uint32_t bg_pixel;
  uint32_t border_pixel;
  uint32_t colormap;
  uint32_t visual;
  uint8_t* fb;                // RGBA8, w*h*4, byte0=R .. byte3=A
  unsigned seq;               // creation order (stacking approximation)
} Drw;

typedef struct {
  uint32_t id;
  int      used;
  uint32_t fg, bg;
  uint32_t function;
  uint32_t line_width;
  uint32_t fill_style;
  uint32_t plane_mask;
  uint32_t arc_mode;
  uint32_t drawable;
  int      graphics_exposures;
} GCRec;

typedef struct { uint32_t win, atom, type; uint8_t format; uint32_t len; uint8_t* data; int used; } Prop;

static Drw    g_drw[MAX_DRAWABLES];
static GCRec  g_gc[MAX_GCS];
static Prop   g_prop[MAX_PROPS];
static char*  g_atom[MAX_ATOMS];
static int    g_natoms;
static uint32_t g_cmaps[MAX_CMAPS];
static int    g_ncmaps;
static unsigned g_drw_seq;

static int      g_cfd = -1;           // client fd
static int      g_lfd = -1;           // listen fd
static pthread_t g_thread;
static volatile int g_running;
static char     g_sockpath[256];

static uint32_t g_seq;                // sequence number of the last request processed
static int      g_bigreq;             // BIG-REQUESTS enabled
static int      g_dirty;              // needs present
static uint32_t g_present_win;
static int      g_opened;
static uint32_t g_open_w, g_open_h;
static uint8_t* g_snap; static size_t g_snap_cap;

// last presented frame, kept so tests / the render bridge can re-present.
static uint32_t g_frames;

// ---------------------------------------------------------------------------
// output buffering
// ---------------------------------------------------------------------------
static uint8_t* g_ob; static size_t g_ob_len, g_ob_cap;

static void ob_need(size_t n) {
  if (g_ob_len + n <= g_ob_cap) return;
  size_t cap = g_ob_cap ? g_ob_cap : 8192;
  while (cap < g_ob_len + n) cap *= 2;
  uint8_t* nb = (uint8_t*)realloc(g_ob, cap);
  if (!nb) { xwarn("out of memory (obuf %zu)", cap); return; }
  g_ob = nb; g_ob_cap = cap;
}
static void ob_put(const void* p, size_t n) {
  if (!n) return;
  ob_need(n); if (g_ob_len + n > g_ob_cap) return;
  memcpy(g_ob + g_ob_len, p, n); g_ob_len += n;
}
static void ob_zero(size_t n) {
  if (!n) return;
  ob_need(n); if (g_ob_len + n > g_ob_cap) return;
  memset(g_ob + g_ob_len, 0, n); g_ob_len += n;
}
static void ob_flush(void) {
  size_t off = 0;
  while (off < g_ob_len && g_cfd >= 0) {
    ssize_t k = write(g_cfd, g_ob + off, g_ob_len - off);
    if (k > 0) { off += (size_t)k; continue; }
    if (k < 0 && (errno == EINTR)) continue;
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd pf = { g_cfd, POLLOUT, 0 };
      poll(&pf, 1, 200); continue;
    }
    break;                              // client gone
  }
  g_ob_len = 0;
}

// A reply: 32-byte fixed part (bytes 8..31 supplied by the caller in body24) plus
// an optional tail which is padded to a 4-byte multiple; length = padded tail / 4.
static void reply(uint8_t detail, const uint8_t* body24, const void* tail, size_t tail_len) {
  uint8_t h[32];
  memset(h, 0, sizeof h);
  h[0] = 1; h[1] = detail;
  p16(h + 2, (uint16_t)g_seq);
  size_t pad = PAD4(tail_len) - tail_len;
  p32(h + 4, (uint32_t)((tail_len + pad) / 4));
  if (body24) memcpy(h + 8, body24, 24);
  ob_put(h, 32);
  if (tail_len) ob_put(tail, tail_len);
  if (pad) ob_zero(pad);
}

static void x_error(uint8_t code, uint32_t bad, uint8_t major, uint16_t minor) {
  uint8_t e[32];
  memset(e, 0, sizeof e);
  e[0] = 0; e[1] = code;
  p16(e + 2, (uint16_t)g_seq);
  p32(e + 4, bad);
  p16(e + 8, minor);
  e[10] = major;
  ob_put(e, 32);
  xlog("error code=%u bad=0x%x major=%u", code, bad, major);
}

static void send_event(const uint8_t ev[32]) { ob_put(ev, 32); }

// ---------------------------------------------------------------------------
// resource tables
// ---------------------------------------------------------------------------
static Drw* d_find(uint32_t id) {
  if (!id) return NULL;
  for (int i = 0; i < MAX_DRAWABLES; i++) if (g_drw[i].used && g_drw[i].id == id) return &g_drw[i];
  return NULL;
}
static Drw* d_new(uint32_t id) {
  Drw* d = d_find(id);
  if (d) { free(d->fb); memset(d, 0, sizeof *d); }
  else { for (int i = 0; i < MAX_DRAWABLES; i++) if (!g_drw[i].used) { d = &g_drw[i]; break; } }
  if (!d) return NULL;
  memset(d, 0, sizeof *d);
  d->used = 1; d->id = id; d->seq = ++g_drw_seq;
  return d;
}
static void d_free(Drw* d) {
  if (!d) return;
  free(d->fb); memset(d, 0, sizeof *d);
}

static GCRec* gc_find(uint32_t id) {
  for (int i = 0; i < MAX_GCS; i++) if (g_gc[i].used && g_gc[i].id == id) return &g_gc[i];
  return NULL;
}
static GCRec* gc_new(uint32_t id) {
  GCRec* g = gc_find(id);
  if (!g) { for (int i = 0; i < MAX_GCS; i++) if (!g_gc[i].used) { g = &g_gc[i]; break; } }
  if (!g) return NULL;
  memset(g, 0, sizeof *g);
  g->used = 1; g->id = id; g->fg = 0x000000; g->bg = 0xFFFFFF;
  g->line_width = 1; g->function = 3 /*GXcopy*/; g->plane_mask = 0xFFFFFFFFu;
  return g;
}

// ---------------------------------------------------------------------------
// atoms
// ---------------------------------------------------------------------------
static const char* const kPredefAtoms[] = {
  "PRIMARY","SECONDARY","ARC","ATOM","BITMAP","CARDINAL","COLORMAP","CURSOR",
  "CUT_BUFFER0","CUT_BUFFER1","CUT_BUFFER2","CUT_BUFFER3","CUT_BUFFER4","CUT_BUFFER5",
  "CUT_BUFFER6","CUT_BUFFER7","DRAWABLE","FONT","INTEGER","PIXMAP","POINT","RECTANGLE",
  "RESOURCE_MANAGER","RGB_COLOR_MAP","RGB_BEST_MAP","RGB_BLUE_MAP","RGB_DEFAULT_MAP",
  "RGB_GRAY_MAP","RGB_GREEN_MAP","RGB_RED_MAP","STRING","VISUALID","WINDOW","WM_COMMAND",
  "WM_HINTS","WM_CLIENT_MACHINE","WM_ICON_NAME","WM_ICON_SIZE","WM_NAME","WM_NORMAL_HINTS",
  "WM_SIZE_HINTS","WM_ZOOM_HINTS","MIN_SPACE","NORM_SPACE","MAX_SPACE","END_SPACE",
  "SUPERSCRIPT_X","SUPERSCRIPT_Y","SUBSCRIPT_X","SUBSCRIPT_Y","UNDERLINE_POSITION",
  "UNDERLINE_THICKNESS","STRIKEOUT_ASCENT","STRIKEOUT_DESCENT","ITALIC_ANGLE","X_HEIGHT",
  "QUAD_WIDTH","WEIGHT","POINT_SIZE","RESOLUTION","COPYRIGHT","NOTICE","FONT_NAME",
  "FAMILY_NAME","FULL_NAME","CAP_HEIGHT","WM_CLASS","WM_TRANSIENT_FOR"
};
#define NPREDEF ((int)(sizeof(kPredefAtoms)/sizeof(kPredefAtoms[0])))

static void atoms_init(void) {
  if (g_natoms) return;
  for (int i = 0; i < NPREDEF; i++) g_atom[i + 1] = strdup(kPredefAtoms[i]);
  g_natoms = NPREDEF;                                   // atoms 1..68
}
static uint32_t atom_intern(const char* name, size_t n, int only_if_exists) {
  for (int i = 1; i <= g_natoms; i++)
    if (g_atom[i] && strlen(g_atom[i]) == n && !memcmp(g_atom[i], name, n)) return (uint32_t)i;
  if (only_if_exists) return 0;
  if (g_natoms + 1 >= MAX_ATOMS) return 0;
  int id = ++g_natoms;
  g_atom[id] = (char*)malloc(n + 1);
  if (!g_atom[id]) { g_natoms--; return 0; }
  memcpy(g_atom[id], name, n); g_atom[id][n] = 0;
  return (uint32_t)id;
}
static const char* atom_name(uint32_t a) {
  if (a == 0 || (int)a > g_natoms) return NULL;
  return g_atom[a];
}

// ---------------------------------------------------------------------------
// properties
// ---------------------------------------------------------------------------
static Prop* prop_find(uint32_t win, uint32_t atom) {
  for (int i = 0; i < MAX_PROPS; i++)
    if (g_prop[i].used && g_prop[i].win == win && g_prop[i].atom == atom) return &g_prop[i];
  return NULL;
}
static void prop_set(uint32_t win, uint32_t atom, uint32_t type, uint8_t format,
                     const uint8_t* data, uint32_t nbytes, uint8_t mode) {
  Prop* p = prop_find(win, atom);
  if (!p) {
    for (int i = 0; i < MAX_PROPS; i++) if (!g_prop[i].used) { p = &g_prop[i]; break; }
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->used = 1; p->win = win; p->atom = atom; mode = 0;
  }
  if (mode == 0 /*Replace*/) {
    free(p->data);
    p->data = (uint8_t*)malloc(nbytes ? nbytes : 1);
    if (p->data && nbytes) memcpy(p->data, data, nbytes);
    p->len = nbytes;
  } else {                                              // Prepend(1) / Append(2)
    uint8_t* nd = (uint8_t*)malloc(p->len + nbytes + 1);
    if (!nd) return;
    if (mode == 2) { memcpy(nd, p->data, p->len); memcpy(nd + p->len, data, nbytes); }
    else           { memcpy(nd, data, nbytes); memcpy(nd + nbytes, p->data, p->len); }
    free(p->data); p->data = nd; p->len += nbytes;
  }
  p->type = type; p->format = format;
}
static void prop_delete(uint32_t win, uint32_t atom) {
  Prop* p = prop_find(win, atom);
  if (!p) return;
  free(p->data); memset(p, 0, sizeof *p);
}

// ---------------------------------------------------------------------------
// framebuffer helpers
// ---------------------------------------------------------------------------
static inline uint32_t rgba_of(uint32_t pixel) {
  uint32_t r = (pixel >> 16) & 0xFF, g = (pixel >> 8) & 0xFF, b = pixel & 0xFF;
  return r | (g << 8) | (b << 16) | 0xFF000000u;        // little-endian: byte0=R .. byte3=A
}
static inline uint32_t pixel_of(uint32_t rgba) {
  uint32_t r = rgba & 0xFF, g = (rgba >> 8) & 0xFF, b = (rgba >> 16) & 0xFF;
  return (r << 16) | (g << 8) | b;
}

static void fb_alloc(Drw* d, uint32_t fill) {
  size_t n = (size_t)d->w * d->h;
  free(d->fb);
  d->fb = NULL;
  if (!n || d->class_ == 2) return;
  d->fb = (uint8_t*)malloc(n * 4);
  if (!d->fb) { xwarn("fb alloc failed %ux%u", d->w, d->h); return; }
  uint32_t* q = (uint32_t*)d->fb;
  uint32_t v = rgba_of(fill);
  for (size_t i = 0; i < n; i++) q[i] = v;
}

static inline void px_set(Drw* d, int x, int y, uint32_t rgba) {
  if (!d->fb || x < 0 || y < 0 || x >= (int)d->w || y >= (int)d->h) return;
  ((uint32_t*)d->fb)[(size_t)y * d->w + (size_t)x] = rgba;
}
static inline uint32_t px_get(const Drw* d, int x, int y) {
  if (!d->fb || x < 0 || y < 0 || x >= (int)d->w || y >= (int)d->h) return 0xFF000000u;
  return ((const uint32_t*)d->fb)[(size_t)y * d->w + (size_t)x];
}
// honours a small subset of GC functions (GXcopy default, GXxor used by rubber-banding)
static inline void px_op(Drw* d, const GCRec* gc, int x, int y, uint32_t rgba) {
  if (gc && gc->function == 6 /*GXxor*/) {
    uint32_t o = px_get(d, x, y);
    px_set(d, x, y, ((o ^ rgba) & 0x00FFFFFFu) | 0xFF000000u);
    return;
  }
  px_set(d, x, y, rgba);
}

static void fill_rect(Drw* d, const GCRec* gc, int x, int y, int w, int h, uint32_t rgba) {
  if (w <= 0 || h <= 0) return;
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++) px_op(d, gc, xx, yy, rgba);
}

static void draw_line(Drw* d, const GCRec* gc, int x0, int y0, int x1, int y1, uint32_t rgba) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  int lw = gc && gc->line_width > 1 ? (int)gc->line_width : 1;
  for (;;) {
    if (lw <= 1) px_op(d, gc, x0, y0, rgba);
    else {
      int h = lw / 2;
      for (int j = -h; j <= h; j++) for (int i = -h; i <= h; i++) px_op(d, gc, x0 + i, y0 + j, rgba);
    }
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// X arcs: (x,y,w,h) is the bounding box; angles are 1/64 degree, CCW from 3 o'clock.
static int arc_angle_ok(double dx, double dy, double a1, double a2) {
  if (a2 >= 360.0 || a2 <= -360.0) return 1;
  double ang = atan2(-dy, dx) * 180.0 / M_PI;           // y grows downward on screen
  if (ang < 0) ang += 360.0;
  double lo, hi;
  if (a2 >= 0) { lo = a1; hi = a1 + a2; } else { lo = a1 + a2; hi = a1; }
  while (lo < 0)   { lo += 360.0; hi += 360.0; }
  while (lo >= 360.0) { lo -= 360.0; hi -= 360.0; }
  if (ang >= lo && ang <= hi) return 1;
  if (ang + 360.0 >= lo && ang + 360.0 <= hi) return 1;
  return 0;
}

static void fill_arc(Drw* d, const GCRec* gc, int ax, int ay, int aw, int ah,
                     int a1_64, int a2_64, uint32_t rgba) {
  if (aw <= 0 || ah <= 0) return;
  double rx = aw / 2.0, ry = ah / 2.0;
  double cx = ax + rx, cy = ay + ry;
  double a1 = a1_64 / 64.0, a2 = a2_64 / 64.0;
  int full = (a2 >= 360.0 || a2 <= -360.0);
  for (int py = ay; py < ay + ah; py++) {
    if (py < 0 || py >= (int)d->h) continue;
    double dy = (py + 0.5) - cy;
    double t = 1.0 - (dy * dy) / (ry * ry);
    if (t < 0) continue;
    double half = rx * sqrt(t);
    int x0 = (int)floor(cx - half), x1 = (int)ceil(cx + half);
    for (int px = x0; px <= x1; px++) {
      double dx = (px + 0.5) - cx;
      if ((dx * dx) / (rx * rx) + (dy * dy) / (ry * ry) > 1.0) continue;
      if (!full && !arc_angle_ok(dx, dy, a1, a2)) continue;
      px_op(d, gc, px, py, rgba);
    }
  }
}

static void stroke_arc(Drw* d, const GCRec* gc, int ax, int ay, int aw, int ah,
                       int a1_64, int a2_64, uint32_t rgba) {
  if (aw <= 0 || ah <= 0) return;
  double rx = aw / 2.0, ry = ah / 2.0;
  double cx = ax + rx, cy = ay + ry;
  double a1 = a1_64 / 64.0, a2 = a2_64 / 64.0;
  int full = (a2 >= 360.0 || a2 <= -360.0);
  double lw = gc && gc->line_width > 1 ? (double)gc->line_width : 1.0;
  double irx = rx - lw, iry = ry - lw;
  if (irx < 0) irx = 0;
  if (iry < 0) iry = 0;
  for (int py = ay - 1; py <= ay + ah; py++) {
    for (int px = ax - 1; px <= ax + aw; px++) {
      double dx = (px + 0.5) - cx, dy = (py + 0.5) - cy;
      double o = (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry);
      if (o > 1.0) continue;
      if (irx > 0.5 && iry > 0.5) {
        double in = (dx * dx) / (irx * irx) + (dy * dy) / (iry * iry);
        if (in <= 1.0) continue;
      }
      if (!full && !arc_angle_ok(dx, dy, a1, a2)) continue;
      px_op(d, gc, px, py, rgba);
    }
  }
}

// even-odd scanline polygon fill
static void fill_poly(Drw* d, const GCRec* gc, const int32_t* pts, int n, uint32_t rgba) {
  if (n < 3) return;
  int miny = pts[1], maxy = pts[1];
  for (int i = 1; i < n; i++) { int y = pts[i * 2 + 1]; if (y < miny) miny = y; if (y > maxy) maxy = y; }
  if (miny < 0) miny = 0;
  if (maxy >= (int)d->h) maxy = (int)d->h - 1;
  double* xs = (double*)malloc(sizeof(double) * (size_t)n + sizeof(double));
  if (!xs) return;
  for (int y = miny; y <= maxy; y++) {
    double yc = y + 0.5;
    int cnt = 0;
    for (int i = 0; i < n; i++) {
      int j = (i + 1) % n;
      double y0 = pts[i * 2 + 1], y1 = pts[j * 2 + 1];
      double x0 = pts[i * 2],     x1 = pts[j * 2];
      if ((y0 <= yc && y1 > yc) || (y1 <= yc && y0 > yc)) {
        double t = (yc - y0) / (y1 - y0);
        xs[cnt++] = x0 + t * (x1 - x0);
      }
    }
    for (int a = 1; a < cnt; a++) { double v = xs[a]; int b = a - 1; while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; } xs[b + 1] = v; }
    for (int a = 0; a + 1 < cnt; a += 2) {
      int sx = (int)ceil(xs[a] - 0.5), ex = (int)floor(xs[a + 1] - 0.5);
      for (int x = sx; x <= ex; x++) px_op(d, gc, x, y, rgba);
    }
  }
  free(xs);
}

// ---------------------------------------------------------------------------
// 8x8 bitmap font (MSB = leftmost pixel), covering 0x20..0x5F; lowercase folds up.
// ---------------------------------------------------------------------------
static const uint8_t g_font[64][8] = {
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
  {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, /* ! */
  {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, /* " */
  {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* # */
  {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, /* $ */
  {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* % */
  {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* & */
  {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* ' */
  {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* ( */
  {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* ) */
  {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
  {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* + */
  {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* , */
  {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* - */
  {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . */
  {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* / */
  {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, /* 0 */
  {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 1 */
  {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00}, /* 2 */
  {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, /* 3 */
  {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* 4 */
  {0xFE,0xC0,0xC0,0xFC,0x06,0xC6,0x7C,0x00}, /* 5 */
  {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, /* 6 */
  {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 7 */
  {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, /* 8 */
  {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, /* 9 */
  {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* : */
  {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* ; */
  {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* < */
  {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* = */
  {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* > */
  {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, /* ? */
  {0x7C,0xC6,0xDE,0xDE,0xDC,0xC0,0x7C,0x00}, /* @ */
  {0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00}, /* A */
  {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* B */
  {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* C */
  {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* D */
  {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* E */
  {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* F */
  {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, /* G */
  {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* H */
  {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* I */
  {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* J */
  {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* K */
  {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* L */
  {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* M */
  {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* N */
  {0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* O */
  {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* P */
  {0x78,0xCC,0xCC,0xCC,0xDC,0x78,0x1C,0x00}, /* Q */
  {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* R */
  {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, /* S */
  {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00}, /* T */
  {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* U */
  {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* V */
  {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, /* W */
  {0xC6,0x6C,0x38,0x38,0x6C,0xC6,0xC6,0x00}, /* X */
  {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, /* Y */
  {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* Z */
  {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* [ */
  {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* \ */
  {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* ] */
  {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* ^ */
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
};
#define FONT_W 8
#define FONT_H 8
#define FONT_ASCENT  7
#define FONT_DESCENT 1

static const uint8_t* glyph_of(unsigned char c) {
  if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
  if (c < 0x20 || c > 0x5F) c = '?';
  return g_font[c - 0x20];
}

// draw_text: baseline at y (X semantics); fill_bg != 0 for ImageText8.
static void draw_text(Drw* d, const GCRec* gc, int x, int y,
                      const uint8_t* s, int n, int fill_bg) {
  uint32_t fg = rgba_of(gc ? gc->fg : 0), bg = rgba_of(gc ? gc->bg : 0xFFFFFF);
  int top = y - FONT_ASCENT;
  if (fill_bg) fill_rect(d, NULL, x, top, n * FONT_W, FONT_H, bg);
  for (int i = 0; i < n; i++) {
    const uint8_t* gl = glyph_of(s[i]);
    for (int r = 0; r < FONT_H; r++) {
      uint8_t bits = gl[r];
      for (int c = 0; c < FONT_W; c++)
        if (bits & (0x80 >> c)) px_op(d, gc, x + i * FONT_W + c, top + r, fg);
    }
  }
}

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------
static void ev_expose(const Drw* d) {
  if (!(d->event_mask & EV_EXPOSURE)) return;
  uint8_t e[32]; memset(e, 0, sizeof e);
  e[0] = XEV_EXPOSE; p16(e + 2, (uint16_t)g_seq);
  p32(e + 4, d->id);
  p16(e + 8, 0); p16(e + 10, 0);
  p16(e + 12, (uint16_t)d->w); p16(e + 14, (uint16_t)d->h);
  p16(e + 16, 0);                                     // count
  send_event(e);
}
static void ev_map(const Drw* d) {
  uint8_t e[32]; memset(e, 0, sizeof e);
  e[0] = XEV_MAPNOTIFY; p16(e + 2, (uint16_t)g_seq);
  p32(e + 4, d->id); p32(e + 8, d->id);
  e[12] = (uint8_t)d->override_redirect;
  if (d->event_mask & EV_STRUCTURE) send_event(e);
  Drw* par = d_find(d->parent);
  if (par && (par->event_mask & EV_SUBSTRUCTURE)) { p32(e + 4, par->id); send_event(e); }
}
static void ev_unmap(const Drw* d) {
  uint8_t e[32]; memset(e, 0, sizeof e);
  e[0] = XEV_UNMAPNOTIFY; p16(e + 2, (uint16_t)g_seq);
  p32(e + 4, d->id); p32(e + 8, d->id);
  if (d->event_mask & EV_STRUCTURE) send_event(e);
  Drw* par = d_find(d->parent);
  if (par && (par->event_mask & EV_SUBSTRUCTURE)) { p32(e + 4, par->id); send_event(e); }
}
static void ev_configure(const Drw* d) {
  uint8_t e[32]; memset(e, 0, sizeof e);
  e[0] = XEV_CONFIGURENOTIFY; p16(e + 2, (uint16_t)g_seq);
  p32(e + 4, d->id); p32(e + 8, d->id); p32(e + 12, 0);
  p16(e + 16, (uint16_t)d->x); p16(e + 18, (uint16_t)d->y);
  p16(e + 20, (uint16_t)d->w); p16(e + 22, (uint16_t)d->h);
  p16(e + 24, d->border_width);
  e[26] = (uint8_t)d->override_redirect;
  if (d->event_mask & EV_STRUCTURE) send_event(e);
  Drw* par = d_find(d->parent);
  if (par && (par->event_mask & EV_SUBSTRUCTURE)) { p32(e + 4, par->id); send_event(e); }
}

// MapWindow / MapSubwindows both funnel through here: mark mapped, MapNotify, ConfigureNotify,
// Expose.
//
// The ConfigureNotify is not optional in practice. On a normal desktop a window manager sends a
// synthetic one when it maps and places a window, and toolkits take their size from it. There is
// no window manager here, so without this a client is mapped and told nothing about its own
// geometry -- Blender selects StructureNotify, never hears back, and sits in its event loop with
// a window it will not paint.
static void emit_map_events(Drw* d) {
  if (d->mapped) return;
  d->mapped = 1;
  ev_map(d);
  ev_configure(d);
  ev_expose(d);
  g_dirty = 1;
}

// ---------------------------------------------------------------------------
// presentation
// ---------------------------------------------------------------------------
static void blit_into(uint8_t* dst, int dw, int dh, const Drw* src, int ox, int oy) {
  if (!src->fb) return;
  for (int y = 0; y < (int)src->h; y++) {
    int dy = oy + y;
    if (dy < 0 || dy >= dh) continue;
    const uint32_t* s = (const uint32_t*)src->fb + (size_t)y * src->w;
    uint32_t* q = (uint32_t*)dst + (size_t)dy * dw;
    for (int x = 0; x < (int)src->w; x++) {
      int dx = ox + x;
      if (dx < 0 || dx >= dw) continue;
      q[dx] = s[x];
    }
  }
}

// Toolkit apps draw into CHILD widget windows; blit every mapped descendant onto
// the snapshot at its parent-relative offset.  Without this the window is blank.
static void composite_children(uint8_t* dst, int dw, int dh, uint32_t parent, int ox, int oy) {
  for (unsigned pass = 1; pass <= g_drw_seq; pass++) {
    for (int i = 0; i < MAX_DRAWABLES; i++) {
      Drw* c = &g_drw[i];
      if (!c->used || !c->is_window || c->parent != parent || c->seq != pass) continue;
      if (!c->mapped || c->class_ == 2) continue;
      int cx = ox + c->x, cy = oy + c->y;
      blit_into(dst, dw, dh, c, cx, cy);
      composite_children(dst, dw, dh, c->id, cx, cy);
    }
  }
}

static Drw* pick_present_window(void) {
  Drw* cur = d_find(g_present_win);
  if (cur && cur->used && cur->is_window && cur->mapped) return cur;
  Drw* best = NULL;
  for (int i = 0; i < MAX_DRAWABLES; i++) {
    Drw* d = &g_drw[i];
    if (!d->used || !d->is_window || !d->mapped || d->class_ == 2) continue;
    if (d->parent != ROOT_WIN) continue;
    if (!best || d->seq < best->seq) best = d;
  }
  if (best) g_present_win = best->id;
  return best;
}

// Defined with the rest of the GL statistics below; the CPU present path reports through the
// same histogram so xeyes and glxgears can be compared on one scale.
static int    glstat_on(void);
static double glx_now_ms(void);
static void   glstat_frame(double now, double t_sub, double t_pre);

static void do_present(void) {
  Drw* top = pick_present_window();
  if (!top || !top->fb || !top->w || !top->h) return;
  size_t need = (size_t)top->w * top->h * 4;
  if (need > g_snap_cap) {
    uint8_t* nb = (uint8_t*)realloc(g_snap, need);
    if (!nb) return;
    g_snap = nb; g_snap_cap = need;
  }
  double t_a = glstat_on() ? glx_now_ms() : 0.0;
  memcpy(g_snap, top->fb, need);
  composite_children(g_snap, (int)top->w, (int)top->h, top->id, 0, 0);
  double t_b = glstat_on() ? glx_now_ms() : 0.0;

  if (!g_opened || g_open_w != top->w || g_open_h != top->h) {
    x11rnd_open(top->w, top->h);
    g_opened = 1; g_open_w = top->w; g_open_h = top->h;
  }
  x11rnd_present(g_snap, top->w, top->h);
  // X11SRV_DUMP=<prefix> writes presented frames as binary PPM, so what the server actually
  // produced can be checked without looking at the screen (the Wayland side has the same knob).
  {
    static int dumped;
    const char* dump = getenv("X11SRV_DUMP");
    if (dump && dumped < 8) {
      char path[512]; snprintf(path, sizeof path, "%s%03d.ppm", dump, dumped);
      FILE* f = fopen(path, "wb");
      if (f) {
        fprintf(f, "P6\n%u %u\n255\n", top->w, top->h);
        for (size_t i = 0; i < (size_t)top->w * top->h; i++) fwrite(g_snap + i * 4, 1, 3, f);
        fclose(f);
        dumped++;
      }
    }
  }
  if (glstat_on()) glstat_frame(glx_now_ms(), t_b - t_a, glx_now_ms() - t_b);
  g_frames++;
  xlog("present #%u %ux%u win=0x%x", g_frames, top->w, top->h, top->id);
}

// ---------------------------------------------------------------------------
// connection setup
// ---------------------------------------------------------------------------
static const char kVendor[] = "rdrift";

// Builds the success setup reply.  The `length` field counts the 4-byte units
// AFTER the first 8 bytes; a wrong value here is the classic cause of
// "xcb: Too much data requested from _XRead".
static void send_setup_reply(void) {
  uint8_t b[512];
  memset(b, 0, sizeof b);
  size_t o = 0;
  const uint16_t vlen = (uint16_t)(sizeof(kVendor) - 1);

  b[0] = 1;                                 // success
  b[1] = 0;
  p16(b + 2, 11); p16(b + 4, 0);            // protocol 11.0
  // b+6 length: filled in below
  p32(b + 8, 1);                            // release number
  p32(b + 12, RID_BASE);
  p32(b + 16, RID_MASK);
  p32(b + 20, 0);                           // motion buffer size
  p16(b + 24, vlen);
  p16(b + 26, 65535);                       // maximum-request-length (units)
  b[28] = 1;                                // number of screens
  b[29] = 3;                                // number of pixmap formats
  b[30] = 0;                                // image-byte-order: LSBFirst
  b[31] = 0;                                // bitmap-format-bit-order: LSBFirst
  b[32] = 32;                               // bitmap-format-scanline-unit
  b[33] = 32;                               // bitmap-format-scanline-pad
  b[34] = 8;                                // min-keycode
  b[35] = 255;                              // max-keycode
  p32(b + 36, 0);                           // pad
  o = 40;

  memcpy(b + o, kVendor, vlen); o += vlen;
  while (o & 3) b[o++] = 0;                 // pad the vendor string

  // LISTofFORMAT: depth, bits-per-pixel, scanline-pad, 5 pad = 8 bytes each
  const uint8_t fmts[3][3] = { {1, 1, 32}, {24, 32, 32}, {32, 32, 32} };
  for (int i = 0; i < 3; i++) {
    b[o + 0] = fmts[i][0]; b[o + 1] = fmts[i][1]; b[o + 2] = fmts[i][2];
    o += 8;
  }

  // SCREEN (40 bytes fixed)
  p32(b + o + 0, ROOT_WIN);
  p32(b + o + 4, ROOT_CMAP);
  p32(b + o + 8, 0x00FFFFFF);               // white pixel
  p32(b + o + 12, 0x00000000);              // black pixel
  p32(b + o + 16, 0);                       // current input masks
  p16(b + o + 20, SCREEN_W);
  p16(b + o + 22, SCREEN_H);
  p16(b + o + 24, 340);                     // width in mm
  p16(b + o + 26, 212);                     // height in mm
  p16(b + o + 28, 1);                       // min installed maps
  p16(b + o + 30, 1);                       // max installed maps
  p32(b + o + 32, VISUAL_24);               // root visual
  b[o + 36] = 0;                            // backing-stores: Never
  b[o + 37] = 0;                            // save-unders
  b[o + 38] = 24;                           // root depth
  b[o + 39] = 2;                            // number of allowed DEPTHs
  o += 40;

  // DEPTH 24 with one TrueColor visual, DEPTH 32 with one TrueColor visual
  const struct { uint8_t depth; uint32_t vid; } deps[2] = { {24, VISUAL_24}, {32, VISUAL_32} };
  for (int i = 0; i < 2; i++) {
    b[o + 0] = deps[i].depth;
    b[o + 1] = 0;
    p16(b + o + 2, 1);                      // one VISUALTYPE
    p32(b + o + 4, 0);
    o += 8;
    p32(b + o + 0, deps[i].vid);
    b[o + 4] = 4;                           // class: TrueColor
    b[o + 5] = 8;                           // bits per rgb value
    p16(b + o + 6, 256);                    // colormap entries
    p32(b + o + 8, 0x00FF0000);             // red mask
    p32(b + o + 12, 0x0000FF00);            // green mask
    p32(b + o + 16, 0x000000FF);            // blue mask
    p32(b + o + 20, 0);                     // pad
    o += 24;
  }

  p16(b + 6, (uint16_t)((o - 8) / 4));      // additional-data length, in 4-byte units
  ob_put(b, o);
  xlog("setup reply: %zu bytes total, length field = %u", o, (unsigned)((o - 8) / 4));
}

// ---------------------------------------------------------------------------
// GLX
// ---------------------------------------------------------------------------
// GLX_* attribute tokens used by GetFBConfigs
#define GLX_USE_GL             1
#define GLX_BUFFER_SIZE        2
#define GLX_LEVEL              3
#define GLX_RGBA               4
#define GLX_DOUBLEBUFFER       5
#define GLX_STEREO             6
#define GLX_AUX_BUFFERS        7
#define GLX_RED_SIZE           8
#define GLX_GREEN_SIZE         9
#define GLX_BLUE_SIZE          10
#define GLX_ALPHA_SIZE         11
#define GLX_DEPTH_SIZE         12
#define GLX_STENCIL_SIZE       13
#define GLX_ACCUM_RED_SIZE     14
#define GLX_ACCUM_GREEN_SIZE   15
#define GLX_ACCUM_BLUE_SIZE    16
#define GLX_ACCUM_ALPHA_SIZE   17
#define GLX_CONFIG_CAVEAT      0x20
#define GLX_X_VISUAL_TYPE      0x22
#define GLX_TRANSPARENT_TYPE   0x23
#define GLX_VISUAL_ID          0x800B
#define GLX_SCREEN             0x800C
#define GLX_DRAWABLE_TYPE      0x8010
#define GLX_RENDER_TYPE        0x8011
#define GLX_X_RENDERABLE       0x8012
#define GLX_FBCONFIG_ID        0x8013
#define GLX_MAX_PBUFFER_WIDTH  0x8016
#define GLX_MAX_PBUFFER_HEIGHT 0x8017
#define GLX_MAX_PBUFFER_PIXELS 0x8018

#define GLX_NONE_TOK           0x8000
#define GLX_TRUE_COLOR_TOK     0x8002
#define GLX_RGBA_BIT           0x00000001
#define GLX_WINDOW_BIT         0x00000001
#define GLX_PBUFFER_BIT        0x00000004

// The set of framebuffer configurations this server advertises.
//
// Why more than one: a client doing DIRECT software rendering (Mesa's drisw, which loads
// swrast_dri.so in-process and XPutImages the result) keeps only those of OUR configs that
// EXACTLY match one of its own driver configs -- see driConvertConfigs()/driConfigEqual() in
// Mesa's dri_common.c. Advertising a single config therefore gives that match a single chance,
// and when it misses, Mesa reports "direct rendering: No" and silently falls back to indirect.
// A stock Xorg offers dozens; this cross product covers the formats swrast actually builds.
//
// The FIRST entry is deliberately the config this server has always advertised, so the indirect
// path (which is what our own GL->Metal bridge renders) keeps selecting exactly what it did
// before -- everything else is appended after it.
struct fbcfg { uint8_t alpha, depth, stencil, dbl; };
static const struct fbcfg kFBCfgs[] = {
  { 0, 24, 8, 1 },                                      // the historical config: keep it first
  { 0, 24, 8, 0 }, { 0, 24, 0, 1 }, { 0, 24, 0, 0 },
  { 0, 16, 8, 1 }, { 0, 16, 8, 0 }, { 0, 16, 0, 1 }, { 0, 16, 0, 0 },
  { 0,  0, 8, 1 }, { 0,  0, 8, 0 }, { 0,  0, 0, 1 }, { 0,  0, 0, 0 },
  { 8, 24, 8, 1 }, { 8, 24, 8, 0 }, { 8, 24, 0, 1 }, { 8, 24, 0, 0 },
  { 8, 16, 8, 1 }, { 8, 16, 8, 0 }, { 8, 16, 0, 1 }, { 8, 16, 0, 0 },
  { 8,  0, 8, 1 }, { 8,  0, 8, 0 }, { 8,  0, 0, 1 }, { 8,  0, 0, 0 },
};
#define NFBCONFIGS ((int)(sizeof(kFBCfgs)/sizeof(kFBCfgs[0])))
#define FBCONFIG_NPAIRS  27

// Write one config's 28 attribute PAIRS (56 words) at `out`. Pair order is not significant --
// the client indexes by tag -- but the set must be complete, because Mesa compares every
// attribute it knows about and a missing one keeps its default.
static void fbconfig_pairs(int i, uint32_t* out) {
  const struct fbcfg* c = &kFBCfgs[i];
  uint32_t bufsz = 24u + c->alpha;                      // r+g+b+a, the GLX definition
  int n = 0;
  #define PAIR(t, v) do { out[n++] = (uint32_t)(t); out[n++] = (uint32_t)(v); } while (0)
  PAIR(GLX_FBCONFIG_ID,        0x00000100 + i);
  PAIR(GLX_VISUAL_ID,          VISUAL_24);
  // GLX_SCREEN is NOT a GetFBConfigs attribute -- the screen is implicit in the request, and
  // Mesa logs "unknown fbconfig attribute from server: tag 0x800c" once per config for it. Sending
  // attributes a client does not recognise is not harmless bookkeeping: it is the server telling
  // the client something it cannot verify, and it buried the real diagnostics under a page of
  // warnings.
  PAIR(GLX_X_VISUAL_TYPE,      GLX_TRUE_COLOR_TOK);
  PAIR(GLX_CONFIG_CAVEAT,      GLX_NONE_TOK);
  PAIR(GLX_TRANSPARENT_TYPE,   GLX_NONE_TOK);
  PAIR(GLX_RENDER_TYPE,        GLX_RGBA_BIT);
  // WINDOW and PBUFFER. Without the pbuffer bit, glXChooseFBConfig with
  // GLX_DRAWABLE_TYPE=GLX_PBUFFER_BIT matches NOTHING -- measured with a 20-line repro: 16
  // configs for every other filter, 0 for that one -- so a client asking for an offscreen config
  // gets none and can never bind a context to a pbuffer. Backed by CreatePbuffer registering a
  // real core drawable with a framebuffer.
  PAIR(GLX_DRAWABLE_TYPE,      GLX_WINDOW_BIT | GLX_PBUFFER_BIT);
  PAIR(GLX_X_RENDERABLE,       1);
  PAIR(GLX_USE_GL,             1);
  PAIR(GLX_RGBA,               1);
  PAIR(GLX_DOUBLEBUFFER,       c->dbl);
  PAIR(GLX_STEREO,             0);
  PAIR(GLX_BUFFER_SIZE,        bufsz);
  PAIR(GLX_LEVEL,              0);
  PAIR(GLX_AUX_BUFFERS,        0);
  PAIR(GLX_RED_SIZE,           8);
  PAIR(GLX_GREEN_SIZE,         8);
  PAIR(GLX_BLUE_SIZE,          8);
  PAIR(GLX_ALPHA_SIZE,         c->alpha);
  PAIR(GLX_DEPTH_SIZE,         c->depth);
  PAIR(GLX_STENCIL_SIZE,       c->stencil);
  PAIR(GLX_ACCUM_RED_SIZE,     0);
  PAIR(GLX_ACCUM_GREEN_SIZE,   0);
  PAIR(GLX_ACCUM_BLUE_SIZE,    0);
  PAIR(GLX_ACCUM_ALPHA_SIZE,   0);
  PAIR(GLX_MAX_PBUFFER_WIDTH,  SCREEN_W);
  PAIR(GLX_MAX_PBUFFER_HEIGHT, SCREEN_H);
  #undef PAIR
}

// 18 POSITIONAL properties, in the order libGL's __glXInitVisualConfig expects.
#define VISCFG_NPROPS 18
static void visual_config_props(int i, uint32_t* out) {
  const struct fbcfg* c = &kFBCfgs[i];
  out[0]  = VISUAL_24;                                  // visualID
  out[1]  = 4;                                          // class = TrueColor
  out[2]  = 1;                                          // rgba
  out[3]  = 8; out[4] = 8; out[5] = 8;                  // red / green / blue size
  out[6]  = c->alpha;                                   // alpha size
  out[7]  = 0; out[8] = 0; out[9] = 0; out[10] = 0;     // accum r/g/b/a
  out[11] = c->dbl;                                     // double buffer
  out[12] = 0;                                          // stereo
  out[13] = 24u + c->alpha;                             // buffer size
  out[14] = c->depth;                                   // depth size
  out[15] = c->stencil;                                 // stencil size
  out[16] = 0;                                          // aux buffers
  out[17] = 0;                                          // level
}

static uint32_t g_glx_ctx_tag;
static uint32_t g_glx_drawable;                         // drawable of the current context
static uint32_t g_glxwin_id, g_glxwin_x;                // glXCreateWindow: GLXWindow -> Window

// glXCreatePbuffer: a GL app that renders offscreen creates one, then asks its size back with
// GetDrawableAttributes. Ignoring CreatePbuffer (as this did) leaves the id unknown, so the size
// query answered "no attributes" and the app concluded the drawable did not exist. Blender does
// exactly this for its second, offscreen context, and gave up with "failed to create drawable"
// -- which is why its window came up grey.
#define GLX_PB_MAX 32
static struct { uint32_t id, w, h; } g_pbuf[GLX_PB_MAX];
static int g_pbuf_n;

static void pbuf_add(uint32_t id, uint32_t w, uint32_t h) {
  for (int i = 0; i < g_pbuf_n; i++)
    if (g_pbuf[i].id == id) { g_pbuf[i].w = w; g_pbuf[i].h = h; return; }
  if (g_pbuf_n < GLX_PB_MAX) { g_pbuf[g_pbuf_n].id = id; g_pbuf[g_pbuf_n].w = w;
                               g_pbuf[g_pbuf_n].h = h; g_pbuf_n++; }
}
static void pbuf_del(uint32_t id) {
  for (int i = 0; i < g_pbuf_n; i++)
    if (g_pbuf[i].id == id) { g_pbuf[i] = g_pbuf[--g_pbuf_n]; return; }
}
static int pbuf_find(uint32_t id, uint32_t* w, uint32_t* h) {
  for (int i = 0; i < g_pbuf_n; i++)
    if (g_pbuf[i].id == id) { if (w) *w = g_pbuf[i].w; if (h) *h = g_pbuf[i].h; return 1; }
  return 0;
}
static double   g_glx_last_present_ms;
static uint32_t g_glx_swaps;

static void glx_string_reply(const char* s) {
  uint32_t n = (uint32_t)strlen(s) + 1;                 // includes the NUL
  uint8_t body[24]; memset(body, 0, sizeof body);
  // offset 8 = pad, offset 12 = n  (xGLXQueryServerStringReply layout)
  p32(body + 4, n);
  reply(0, body, s, n);
}

// A GL single reply carrying `n` 32-bit values.  Mesa's __glXReadReply takes the lone value
// out of the reply header (offset 16) when the reply length is 0 and reads an array out of the
// tail otherwise, so n == 1 must NOT put anything in the tail.
static void gl_get_reply(uint32_t n, const uint32_t* vals) {
  uint8_t body[24]; memset(body, 0, sizeof body);
  p32(body + 4, n);                                     // "size" at reply offset 12
  if (n <= 1) {
    if (n == 1) p32(body + 8, vals[0]);                 // the value, at reply offset 16
    reply(0, body, NULL, 0);
    return;
  }
  uint8_t tail[64];
  if (n > 16) n = 16;
  for (uint32_t i = 0; i < n; i++) p32(tail + i * 4, vals[i]);
  reply(0, body, tail, n * 4);
}

// GL single ops that are VOID: they must get NO reply at all.  Every other GLsop returns a
// value and the client blocks until it arrives — and a reply to a void op desynchronises
// Xlib's reply queue, which is the "works, then mysteriously stalls" failure mode.
static int glsop_is_void(uint8_t s) {
  switch (s) {
    case 101: case 102: case 103:                       // NewList / EndList / DeleteLists
    case 105: case 106:                                 // FeedbackBuffer / SelectBuffer
    case 109: case 110:                                 // PixelStoref / PixelStorei
    case 142:                                           // Flush
    case 144:                                           // DeleteTextures
      return 1;
    default:
      return 0;
  }
}

static double glx_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

// ---------------------------------------------------------------------------
// GL frame statistics (RDGLSTATS=1)
//
// One sample per frame the *producer* completes, split into the three stages it costs:
//   decode  — time inside glxd_render/glxd_render_large for the commands of this frame
//   submit  — glxd_swap(): glb_swap() plus, on the fallback path, glb_readback()
//   present — handing the finished frame to the window layer
// Percentiles matter more than the mean: an even 60 Hz and a 1-2-1-2 refresh beat pattern have
// the same average and look completely different.
//
// NOTE this counts frames DRAWN, not frames shown: on the GL fast path the client is free to
// run ahead and have superseded frames dropped, so the rate here is legitimately far above the
// refresh rate. What actually reached the display is the "[gpuwin] PRESENTED" line, measured
// from -[MTLDrawable presentedTime] in window.m.
// ---------------------------------------------------------------------------
#define GLSTAT_CAP 8192
static int      g_glstat_on = -1;
static float    g_glstat_frame[GLSTAT_CAP];
static float    g_glstat_dec[GLSTAT_CAP], g_glstat_sub[GLSTAT_CAP], g_glstat_pre[GLSTAT_CAP];
static int      g_glstat_n;
static double   g_glstat_decacc;       // decode ms accumulated since the last swap
static double   g_glstat_subacc;       // extra submit ms charged by the caller
static double   g_glstat_prev;         // timestamp of the previous swap
static double   g_glstat_t0;           // start of the reporting window

static int glstat_on(void) {
  if (g_glstat_on < 0) {
    const char* e = getenv("RDGLSTATS");
    g_glstat_on = (e && *e && *e != '0');
  }
  return g_glstat_on;
}

static int glstat_cmp(const void* a, const void* b) {
  float x = *(const float*)a, y = *(const float*)b;
  return (x < y) ? -1 : (x > y);
}

static float glstat_pct(float* sorted, int n, double p) {
  if (n <= 0) return 0.0f;
  int i = (int)(p * (double)(n - 1) + 0.5);
  if (i < 0) i = 0;
  if (i >= n) i = n - 1;
  return sorted[i];
}

static double glstat_mean(const float* v, int n) {
  double s = 0;
  for (int i = 0; i < n; i++) s += v[i];
  return n ? s / n : 0.0;
}

static void glstat_report(double now) {
  int n = g_glstat_n;
  if (n < 2) return;
  static float tmp[GLSTAT_CAP];
  memcpy(tmp, g_glstat_frame, sizeof(float) * (size_t)n);
  qsort(tmp, (size_t)n, sizeof(float), glstat_cmp);
  double secs = (now - g_glstat_t0) / 1000.0;
  fprintf(stderr,
          "[glstats] %d frames in %.2fs = %.1f FPS | frame ms p50 %.2f p95 %.2f p99 %.2f "
          "min %.2f max %.2f | decode %.3f submit %.3f present %.3f (mean ms)\n",
          n, secs, secs > 0 ? n / secs : 0.0,
          glstat_pct(tmp, n, 0.50), glstat_pct(tmp, n, 0.95), glstat_pct(tmp, n, 0.99),
          tmp[0], tmp[n - 1],
          glstat_mean(g_glstat_dec, n), glstat_mean(g_glstat_sub, n),
          glstat_mean(g_glstat_pre, n));

  // Histogram of the inter-present interval as a MULTIPLE OF THE DISPLAY REFRESH. Smooth means
  // essentially everything in one bucket; a 1-2-1-2 beat shows as two fat neighbouring buckets.
  {
    double refresh = glstat_pct(tmp, n, 0.50);          // p50 is the display's period
    if (refresh > 0.5) {
      int bucket[9]; memset(bucket, 0, sizeof bucket);  // 0.5x .. 4x+ in half-refresh steps
      for (int i = 0; i < n; i++) {
        int b = (int)(g_glstat_frame[i] / refresh * 2.0 + 0.5) - 1;   // 1.0x -> bucket 1
        if (b < 0) b = 0;
        if (b > 8) b = 8;
        bucket[b]++;
      }
      fprintf(stderr, "[glstats] refresh multiples:");
      for (int b = 0; b < 9; b++)
        if (bucket[b]) fprintf(stderr, "  %.1fx:%.1f%%", (b + 1) * 0.5,
                               100.0 * bucket[b] / n);
      fputc('\n', stderr);
    }
  }
  g_glstat_n = 0;
  g_glstat_t0 = now;
}

// Record one presented GL frame.  `t_swap`/`t_pre` are the stage costs in ms.
static void glstat_frame(double now, double t_sub, double t_pre) {
  if (!glstat_on()) { g_glstat_decacc = g_glstat_subacc = 0; return; }
  if (g_glstat_t0 == 0) {
    g_glstat_t0 = now; g_glstat_prev = now; g_glstat_decacc = g_glstat_subacc = 0;
    return;
  }
  if (g_glstat_n < GLSTAT_CAP) {
    g_glstat_frame[g_glstat_n] = (float)(now - g_glstat_prev);
    g_glstat_dec[g_glstat_n]   = (float)g_glstat_decacc;
    g_glstat_sub[g_glstat_n]   = (float)(t_sub + g_glstat_subacc);
    g_glstat_pre[g_glstat_n]   = (float)t_pre;
    g_glstat_n++;
  }
  g_glstat_prev = now;
  g_glstat_decacc = g_glstat_subacc = 0;
  if (now - g_glstat_t0 >= 5000.0 || g_glstat_n >= GLSTAT_CAP) glstat_report(now);
}

// ---------------------------------------------------------------------------
// The GL fast path.
//
// The slow path costs a full GPU drain plus three whole-frame CPU copies per frame:
//   glb_readback() (waitUntilCompleted + getBytes) -> the window's X framebuffer
//   -> memcpy into the composite snapshot -> replaceRegion upload back onto the GPU.
// When the GL drawable IS the window being presented and nothing is composited on top of it,
// none of that is needed: glbridge's frame is already an MTLTexture, so it can be blitted
// straight into the CAMetalLayer drawable (x11rnd_present_gl -> gpuwin_present_texture).
//
// x11srv_render.m provides the real thing; the weak definition here is what a harness that
// stubs out the render bridge links against, and it simply reports "not available", which
// latches `g_gl_fast` off and leaves the readback path in charge.
// ---------------------------------------------------------------------------
extern int x11rnd_present_gl(uint32_t w, uint32_t h);
__attribute__((weak)) int x11rnd_present_gl(uint32_t w, uint32_t h) {
  (void)w; (void)h;
  return -38;                                      // -ENOSYS
}

static int angle_probe_on(void) {           // RDANGLE, shared with glxdecode.c's probe
  static int v = -1;
  if (v < 0) { const char* e = getenv("RDANGLE"); v = (e && *e && *e != '0'); }
  return v;
}

static int g_gl_fast = -1;                  // -1 = not asked yet, 0 = off, 1 = on
static int gl_fast_enabled(void) {          // RDGLFAST=0 forces the old readback path
  if (g_gl_fast < 0) {
    const char* e = getenv("RDGLFAST");
    g_gl_fast = !(e && *e == '0');
  }
  return g_gl_fast;
}

// True when `w` is exactly what do_present() would put on screen: the presented top-level
// window, with no mapped children that composite_children() would have to blit over it.
static int glx_owns_whole_window(Drw* w) {
  if (!w) return 0;
  if (pick_present_window() != w) return 0;
  for (int i = 0; i < MAX_DRAWABLES; i++) {
    const Drw* c = &g_drw[i];
    if (c->used && c->is_window && c->mapped && c->class_ != 2 && c->parent == w->id) return 0;
  }
  return 1;
}

// The X window the GL context draws into.  glXMakeCurrent hands us either the Window itself
// (glxgears) or a GLXWindow from glXCreateWindow; fall back to the presented window.
static Drw* glx_target(void) {
  uint32_t id = g_glx_drawable;
  if (id && id == g_glxwin_id && g_glxwin_x) id = g_glxwin_x;
  Drw* w = d_find(id);
  if (!w || !w->is_window) w = pick_present_window();
  if (!w || !w->fb || !w->w || !w->h) return NULL;
  return w;
}

// Bring the GL bridge up (and size it to the drawable) before any GL call reaches it.
static void glx_prepare(void) {
  Drw* w = glx_target();
  if (w) glxd_ensure((int)w->w, (int)w->h);
  else   glxd_ensure(64, 64);                           // no drawable yet — just get it alive
}

// Minimal XInput2. Blender queries XInputExtension before anything else it needs, and Xlib's
// "extension XInputExtension missing" was the FIRST failure in its startup log. This answers the
// version handshake and reports the two master devices every XI2 client expects to exist.
//
// SCOPE, deliberately: the devices are reported with ZERO input classes. That is honest -- we do
// not deliver XI2 events -- and it keeps a client from believing it has valuators we will never
// send. A client that wants tablet axes will find none and fall back to core events, which is
// what it does today when the extension is absent altogether.
static void handle_xinput(uint8_t sub, const uint8_t* d, size_t dlen) {
  uint8_t body[24];
  xlog("XI opcode %u (dlen=%u)", (unsigned)sub, (unsigned)dlen);
  switch (sub) {
    case 47: {                                          // XIQueryVersion
      uint16_t want_maj = dlen >= 6 ? g16(d + 4) : 2;
      memset(body, 0, sizeof body);
      p16(body + 0, want_maj >= 2 ? 2 : 1);             // major_version
      p16(body + 2, 0);                                 // minor_version
      reply(0, body, NULL, 0);
      return;
    }
    case 48: {                                          // XIQueryDevice
      static const char* kName[2] = { "Virtual core pointer", "Virtual core keyboard" };
      static const uint16_t kId[2] = { 2, 3 }, kUse[2] = { 1, 2 }, kAtt[2] = { 3, 2 };
      uint8_t tail[128];
      size_t n = 0;
      for (int i = 0; i < 2; i++) {
        size_t nl = strlen(kName[i]), pad = PAD4(nl) - nl;
        p16(tail + n +  0, kId[i]);                     // deviceid
        p16(tail + n +  2, kUse[i]);                    // use
        p16(tail + n +  4, kAtt[i]);                    // attachment
        p16(tail + n +  6, 0);                          // num_classes
        p16(tail + n +  8, (uint16_t)nl);               // name_len
        tail[n + 10] = 1;                               // enabled
        tail[n + 11] = 0;                               // pad
        memcpy(tail + n + 12, kName[i], nl);
        memset(tail + n + 12 + nl, 0, pad);
        n += 12 + nl + pad;
      }
      memset(body, 0, sizeof body);
      p16(body + 0, 2);                                 // num_devices
      reply(0, body, tail, n);
      return;
    }
    case 1: {                                           // XI1 GetExtensionVersion
      // THIS is the one that must never be silently accepted: it expects a REPLY, and a client
      // that does not get one waits for it forever. Blender sends it immediately after
      // QueryExtension succeeds, so swallowing it turned "XInput missing" (a warning it recovers
      // from) into a hang with no window at all -- strictly worse than not offering XInput.
      memset(body, 0, sizeof body);
      p16(body + 0, 2);                                 // server_major
      p16(body + 2, 0);                                 // server_minor
      body[4] = 1;                                      // present
      reply(0, body, NULL, 0);
      return;
    }
    case 2: {                                           // XI1 ListInputDevices
      // Blender does NOT null-check the result: erroring this request made it fault on
      // `ldr x1, [x0]` with x0 = 0, i.e. dereferencing the device list it was never given.
      // Report the two core devices with no classes -- true, and enough to satisfy the walk.
      static const char* kName[2] = { "Virtual core pointer", "Virtual core keyboard" };
      static const uint8_t kId[2] = { 2, 3 }, kUse[2] = { 0, 1 };   // IsXPointer, IsXKeyboard
      uint8_t tail[128];
      size_t n = 0;
      for (int i = 0; i < 2; i++) {                     // xDeviceInfo[]: 8 bytes each
        p32(tail + n, 0);                               // type atom: none
        tail[n + 4] = kId[i];
        tail[n + 5] = 0;                                // num_classes
        tail[n + 6] = kUse[i];
        tail[n + 7] = 0;
        n += 8;
      }
      for (int i = 0; i < 2; i++) {                     // names: length-prefixed, then padded
        size_t l = strlen(kName[i]);
        tail[n++] = (uint8_t)l;
        memcpy(tail + n, kName[i], l);
        n += l;
      }
      while (n & 3) tail[n++] = 0;
      // ndevices is at BYTE 8 of the reply, not in the detail slot: in xListInputDevicesReply
      // byte 1 is RepType (the request code echoed back). Putting the count in the detail slot
      // left byte 8 zero, libXi read "no devices" and returned NULL, and Blender -- which does
      // not null-check -- faulted on `ldr x1, [x0]`. That fault looked identical to the one from
      // erroring the request outright, which is what gave the game away.
      memset(body, 0, sizeof body);
      body[0] = 2;                                      // ndevices
      reply(2 /* RepType = X_ListInputDevices */, body, tail, n);
      xlog("XI ListInputDevices -> 2 devices");
      return;
    }
    case 6:                                             // XI1 SelectExtensionEvent: NO reply
      // Erroring this made Blender's event selection fail, which left it with a null window that
      // it then virtual-dispatched through (`ldr x1,[x0]` with x0 = 0). It was blamed earlier for
      // an XCB desync that was really the malformed ListInputDevices reply below -- with ndevices
      // written into the detail slot instead of byte 8, libXi consumed the wrong length.
    case 46:                                            // XISelectEvents: no reply by definition
      return;
    default:
      // An UNKNOWN request must not be swallowed. Half of them expect a reply, and silence hangs
      // the client; an error unblocks it and lets it fall back. Silence is only correct for the
      // requests known to be reply-less, which are listed explicitly above.
      xlog("XI opcode %u unimplemented -> BadRequest", (unsigned)sub);
      x_error(1 /* BadRequest */, 0, XI_MAJOR, sub);
      return;
  }
}

static void handle_glx(uint8_t sub, const uint8_t* d, size_t dlen) {
  uint8_t body[24];
  // Trace EVERY GLX sub-opcode. Which requests carry an xlog of their own is arbitrary, so a
  // sequence read off those alone is a guess -- it made the traffic look like it stopped at
  // CreateContext when MakeCurrent simply does not log.
  if (sub != 1) xlog("GLX opcode %u (dlen=%u)", (unsigned)sub, (unsigned)dlen);
  switch (sub) {
    case 1:                                             // GLXRender — the GL command batch
      if (dlen >= 4) {
        double t = glstat_on() ? glx_now_ms() : 0.0;
        glx_prepare(); glxd_render(d + 4, dlen - 4);
        if (t) g_glstat_decacc += glx_now_ms() - t;
      }
      return;
    case 2: {                                           // GLXRenderLarge — one split command
      double t = glstat_on() ? glx_now_ms() : 0.0;
      glx_prepare();
      glxd_render_large(d, dlen);
      if (t) g_glstat_decacc += glx_now_ms() - t;
      return;
    }
    case 3:                                             // CreateContext
    case 24:                                            // CreateNewContext
    case 34:                                            // CreateContextAttribsARB
      xlog("GLX CreateContext ctx=0x%x", dlen >= 4 ? g32(d) : 0);
      return;
    case 4:  return;                                    // DestroyContext
    case 5:                                             // MakeCurrent: drawable,context,oldTag
    case 26: {                                          // MakeContextCurrent: oldTag,draw,read,ctx
      uint32_t drw = 0;
      if (sub == 5)  { if (dlen >= 8)  drw = g32(d + 0); }
      else           { if (dlen >= 12) drw = g32(d + 4); }
      if (drw) { g_glx_drawable = drw; glx_prepare(); }
      memset(body, 0, sizeof body);
      if (!g_glx_ctx_tag) g_glx_ctx_tag = 1;
      p32(body + 0, g_glx_ctx_tag);                     // contextTag at reply offset 8
      reply(0, body, NULL, 0);
      xlog("GLX MakeCurrent drawable=0x%x -> tag %u", drw, g_glx_ctx_tag);
      return;
    }
    case 6: {                                           // IsDirect
      memset(body, 0, sizeof body);
      body[0] = 0;                                      // isDirect = False (indirect)
      reply(0, body, NULL, 0);
      return;
    }
    case 7: {                                           // QueryVersion
      memset(body, 0, sizeof body);
      p32(body + 0, 1); p32(body + 4, 4);               // 1.4
      reply(0, body, NULL, 0);
      xlog("GLX QueryVersion -> 1.4");
      return;
    }
    case 8:                                             // WaitGL  — void, no reply
    case 9:                                             // WaitX   — void, no reply
      return;
    case 11: {                                          // SwapBuffers — void, no reply
      if (dlen >= 8 && g32(d + 4)) g_glx_drawable = g32(d + 4);
      Drw* w = glx_target();
      g_glx_swaps++;
      if (!w) { glxd_swap(NULL, 0, 0); return; }
      glxd_ensure((int)w->w, (int)w->h);

      // --- fast path: hand the finished MTLTexture straight to the layer ------------------
      // No throttle and no blocking. The presenter takes the frame only if the previous one has
      // already reached the display, and otherwise drops it (rc == -11) — so presents happen at
      // exactly one per refresh while this thread keeps servicing the client at full speed.
      //
      // That "keep servicing the client" part is load-bearing, and it is why the obvious design
      // (block here until a drawable is free) is wrong. glxgears derives its rotation from the
      // wall clock at DRAW time. If this thread stalls for a refresh, the client does not stop:
      // it draws frames into the socket buffer until that fills, which takes a couple of
      // milliseconds, and then blocks in write() for the ~200 ms it takes us to drain them. All
      // those frames carry almost the same angle, and we then show them one per refresh — so
      // the gears creep for twenty refreshes and lurch on the twenty-first even though frame
      // delivery is metronomic. Measured with RDANGLE: the angle carried by successive
      // presented frames spread 300:1 when this thread blocked, and 1.1:1 when it does not.
      if (gl_fast_enabled() && glx_owns_whole_window(w)) {
        double t_a = glstat_on() ? glx_now_ms() : 0.0;
        glxd_swap(NULL, 0, 0);                          // commit; no readback, no CPU copy
        double t_b = glstat_on() ? glx_now_ms() : 0.0;
        if (!g_opened || g_open_w != w->w || g_open_h != w->h) {
          x11rnd_open(w->w, w->h);
          g_opened = 1; g_open_w = w->w; g_open_h = w->h;
        }
        int rc = x11rnd_present_gl(w->w, w->h);
        if (angle_probe_on())                           // pairs with glxdecode's [swangle] line
          fprintf(stderr, "[shown] %d\n", rc == 0);
        if (rc >= 0 || rc == -11) {                     // -11: superseded, this frame is dropped
          g_frames++;
          g_dirty = 0;
          g_glx_last_present_ms = glx_now_ms();
          if (glstat_on()) glstat_frame(glx_now_ms(), t_b - t_a, glx_now_ms() - t_b);
          xlog("GLX swap #%u fast (%llu GL commands decoded)",
               g_glx_swaps, (unsigned long long)glxd_command_count());
          return;
        }
        xwarn("GL fast present unavailable (%d); falling back to readback", rc);
        g_gl_fast = 0;                                  // latch off; the frame still shows below
      }

      // --- fallback: read the frame back into the window's X framebuffer -----------------
      // A readback drains the GPU and the present is a synchronous hop to the main thread,
      // while glxgears swaps thousands of times a second, so cap it at ~80 Hz.  Skipped frames
      // still go through glb_swap(), so the GL frame accounting stays correct.
      double now = glx_now_ms();
      int show = (now - g_glx_last_present_ms) >= 12.0;
      double t_a = glstat_on() ? glx_now_ms() : 0.0;
      glxd_swap(show ? w->fb : NULL, (int)w->w, (int)w->h);
      double t_b = glstat_on() ? glx_now_ms() : 0.0;
      if (show) {
        g_glx_last_present_ms = now;
        g_glstat_subacc += t_b - t_a;                   // do_present() reports the sample
        do_present();
        g_dirty = 0;
        xlog("GLX swap #%u (%llu GL commands decoded)",
             g_glx_swaps, (unsigned long long)glxd_command_count());
      }
      return;
    }
    case 10: case 12: case 13:
      return;                                           // CopyContext/UseXFont/CreateGLXPixmap
    case 14: {                                          // GetVisualConfigs
      memset(body, 0, sizeof body);
      p32(body + 0, NFBCONFIGS);                        // numVisuals   (offset 8)
      p32(body + 4, VISCFG_NPROPS);                     // numProps     (offset 12)
      uint8_t tail[NFBCONFIGS * VISCFG_NPROPS * 4];
      for (int c = 0; c < NFBCONFIGS; c++) {
        uint32_t props[VISCFG_NPROPS];
        visual_config_props(c, props);
        for (int i = 0; i < VISCFG_NPROPS; i++)
          p32(tail + (c * VISCFG_NPROPS + i) * 4, props[i]);
      }
      reply(0, body, tail, sizeof tail);
      xlog("GLX GetVisualConfigs -> %d visuals x %d props (length=%d words)",
           NFBCONFIGS, VISCFG_NPROPS, NFBCONFIGS * VISCFG_NPROPS);
      return;
    }
    case 15: case 16: return;                           // DestroyGLXPixmap / VendorPrivate
    case 17: {                                          // VendorPrivateWithReply
      memset(body, 0, sizeof body);
      reply(0, body, NULL, 0);
      return;
    }
    case 18:                                            // QueryExtensionsString
      glx_string_reply("");
      return;
    case 19: {                                          // QueryServerString
      uint32_t name = dlen >= 8 ? g32(d + 4) : 0;
      const char* s = "";
      if (name == 1) s = "rdrift";                      // GLX_VENDOR
      else if (name == 2) s = "1.4";                    // GLX_VERSION
      else if (name == 3) s = "";                       // GLX_EXTENSIONS
      glx_string_reply(s);
      xlog("GLX QueryServerString(%u) -> \"%s\"", name, s);
      return;
    }
    case 20:                                            // ClientInfo — no reply
      xlog("GLX ClientInfo %u.%u", dlen >= 8 ? g32(d) : 0, dlen >= 8 ? g32(d + 4) : 0);
      return;
    case 21: {                                          // GetFBConfigs
      // numAttribs is the PAIR COUNT; the data is pairs*2 words.  reply() derives
      // length from the tail size, so the two can never disagree.
      memset(body, 0, sizeof body);
      p32(body + 0, NFBCONFIGS);                        // numFBConfigs (offset 8)
      p32(body + 4, FBCONFIG_NPAIRS);                   // numAttribs   (offset 12) = PAIRS
      uint8_t tail[NFBCONFIGS * FBCONFIG_NPAIRS * 8];
      for (int c = 0; c < NFBCONFIGS; c++) {
        uint32_t pairs[FBCONFIG_NPAIRS * 2];
        fbconfig_pairs(c, pairs);
        for (int i = 0; i < FBCONFIG_NPAIRS * 2; i++)
          p32(tail + (c * FBCONFIG_NPAIRS * 2 + i) * 4, pairs[i]);
      }
      reply(0, body, tail, sizeof tail);
      xlog("GLX GetFBConfigs -> %d configs, numAttribs=%d pairs, length=%d words",
           NFBCONFIGS, FBCONFIG_NPAIRS, NFBCONFIGS * FBCONFIG_NPAIRS * 2);
      return;
    }
    case 22: case 23: return;                           // CreateGLXPixmap / GetDrawableAttributes?
    case 25: {                                          // QueryContext
      memset(body, 0, sizeof body);
      p32(body + 0, 0);                                 // numAttribs = 0
      reply(0, body, NULL, 0);
      return;
    }
    case 31:                                            // CreateWindow: screen,fbcfg,win,glxwin
      if (dlen >= 16) {
        g_glxwin_x  = g32(d + 8);
        g_glxwin_id = g32(d + 12);
        xlog("GLX CreateWindow glxwin=0x%x -> window 0x%x", g_glxwin_id, g_glxwin_x);
      }
      return;
    case 32:                                            // DeleteWindow
      g_glxwin_id = g_glxwin_x = 0;
      return;
    case 27: {                                          // CreatePbuffer: screen,fbcfg,pbuf,nattr
      if (dlen >= 16) {
        uint32_t pb = g32(d + 8), na = g32(d + 12), w = 0, h = 0;
        for (uint32_t i = 0; i < na && 16 + i * 8 + 8 <= dlen; i++) {
          uint32_t t = g32(d + 16 + i * 8), v = g32(d + 16 + i * 8 + 4);
          if (t == 0x8041) w = v;                       // GLX_PBUFFER_WIDTH
          else if (t == 0x8040) h = v;                  // GLX_PBUFFER_HEIGHT
        }
        pbuf_add(pb, w, h);
        // A GLXPbuffer is also a CORE X drawable: a client may XGetGeometry it, and Blender does
        // exactly that immediately after creating one. Registering it only in the GLX table left
        // GetGeometry answering BadDrawable, so Blender destroyed the pbuffer and reported
        // "failed to create drawable" -- the failure was in core X, not in GLX at all.
        if (!d_find(pb)) {
          Drw* pd = d_new(pb);
          if (pd) {
            pd->is_window = 0;
            pd->depth = 24;
            pd->w = w ? w : 1;
            pd->h = h ? h : 1;
            pd->class_ = 1;
            fb_alloc(pd, 0x000000);
          }
        }
        xlog("GLX CreatePbuffer 0x%x %ux%u", pb, w, h);
      }
      return;
    }
    case 28:                                            // DestroyPbuffer
      if (dlen >= 4) {
        uint32_t pb = g32(d + 0);
        pbuf_del(pb);
        Drw* pd = d_find(pb);
        if (pd && !pd->is_window) d_free(pd);
        xlog("GLX DestroyPbuffer 0x%x", pb);
      }
      return;
    case 30: case 33:
      return;                                           // remaining pixmap lifecycle
    case 29: {                                          // GetDrawableAttributes
      memset(body, 0, sizeof body);
      p32(body + 0, 0);
      reply(0, body, NULL, 0);
      return;
    }
    case 129: {                                         // glGetString
      uint32_t name = dlen >= 8 ? g32(d + 4) : 0;
      const char* s;
      switch (name) {
        case 0x1F00: s = "rdrift"; break;                       // GL_VENDOR
        case 0x1F01: s = "rdrift Metal bridge"; break;          // GL_RENDERER
        case 0x1F02: s = "1.4 (rdrift indirect)"; break;        // GL_VERSION
        case 0x1F03: s = ""; break;                             // GL_EXTENSIONS
        default:     s = ""; break;
      }
      uint32_t n = (uint32_t)strlen(s) + 1;
      memset(body, 0, sizeof body);
      p32(body + 0, 0);                                 // retval  (offset 8)
      p32(body + 4, n);                                 // size    (offset 12)
      reply(0, body, s, n);
      xlog("GLX glGetString(0x%x) -> \"%s\"", name, s);
      return;
    }
    // ---- GL single (non-Render) commands -----------------------------------
    // Layout: d+0 is the contextTag, the command's own arguments start at d+4.
    case 101:                                           // glNewList(list, mode) — void
      if (dlen >= 12) { glx_prepare(); glxd_new_list(g32(d + 4), g32(d + 8)); }
      return;
    case 102:                                           // glEndList — void
      glxd_end_list();
      return;
    case 103:                                           // glDeleteLists(list, range) — void
      if (dlen >= 12) glxd_delete_lists(g32(d + 4), g32(d + 8));
      return;
    case 104: {                                         // glGenLists(range) -> base
      uint32_t range = dlen >= 8 ? g32(d + 4) : 1;
      glx_prepare();
      uint32_t base = glxd_gen_lists(range ? range : 1);
      memset(body, 0, sizeof body);
      p32(body + 0, base);                              // retval (reply offset 8)
      reply(0, body, NULL, 0);
      xlog("GLX glGenLists(%u) -> %u", range, base);
      return;
    }
    case 141: {                                         // glIsList(list) -> BOOL
      memset(body, 0, sizeof body);
      p32(body + 0, dlen >= 8 ? (uint32_t)glxd_is_list(g32(d + 4)) : 0u);
      reply(0, body, NULL, 0);
      return;
    }
    case 140: {                                         // glIsEnabled(cap) -> BOOL
      memset(body, 0, sizeof body);
      p32(body + 0, dlen >= 8 ? (uint32_t)glxd_is_enabled(g32(d + 4)) : 0u);
      reply(0, body, NULL, 0);
      return;
    }
    case 115: {                                         // glGetError -> GL_NO_ERROR
      memset(body, 0, sizeof body);
      p32(body + 0, 0);
      reply(0, body, NULL, 0);
      return;
    }
    case 108: {                                         // glFinish — empty reply (a barrier)
      memset(body, 0, sizeof body);
      reply(0, body, NULL, 0);
      return;
    }
    case 107: {                                         // glRenderMode -> retval, no feedback
      memset(body, 0, sizeof body);
      reply(0, body, NULL, 0);
      return;
    }
    case 112: case 114: case 116: case 117: {           // glGetBooleanv/Doublev/Floatv/Integerv
      uint32_t pname = dlen >= 8 ? g32(d + 4) : 0;
      uint32_t v[4] = { 0, 0, 0, 0 };
      uint32_t n = 1;
      switch (pname) {
        case 0x0D31: v[0] = 8; break;                   // GL_MAX_LIGHTS
        case 0x0D33: v[0] = 4096; break;                // GL_MAX_TEXTURE_SIZE
        case 0x0D36: v[0] = 32; break;                  // GL_MAX_MODELVIEW_STACK_DEPTH
        case 0x0D38: v[0] = 4; break;                   // GL_MAX_PROJECTION_STACK_DEPTH
        case 0x0B31: v[0] = 64; break;                  // GL_MAX_LIST_NESTING
        case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: v[0] = 8; break;  // R/G/B/A bits
        case 0x0D56: v[0] = 24; break;                  // GL_DEPTH_BITS
        case 0x0D57: v[0] = 8; break;                   // GL_STENCIL_BITS
        case 0x0BA2: {                                  // GL_VIEWPORT
          Drw* w = glx_target();
          n = 4; v[0] = 0; v[1] = 0;
          v[2] = w ? w->w : 0; v[3] = w ? w->h : 0;
          break;
        }
        default: v[0] = 0; break;
      }
      gl_get_reply(n, v);
      return;
    }
    case 105: case 106: case 109: case 110:             // FeedbackBuffer/SelectBuffer/PixelStore*
    case 142:                                           // glFlush
    case 144:                                           // glDeleteTextures
      return;                                           // all void — send NOTHING

    default:
      if (sub >= 101 && sub <= 160) {
        if (glsop_is_void(sub)) { xlog("GLX single op %u (void, no reply)", sub); return; }
        memset(body, 0, sizeof body);
        reply(0, body, NULL, 0);
        xlog("GLX single op %u unhandled — empty reply", sub);
        return;
      }
      xwarn("GLX sub-opcode %u unhandled (no reply)", sub);
      return;
  }
}

// ---------------------------------------------------------------------------
// value-list decoding helpers
// ---------------------------------------------------------------------------
static uint32_t next_value(const uint8_t** p, const uint8_t* end) {
  if (*p + 4 > end) return 0;
  uint32_t v = g32(*p); *p += 4; return v;
}

static void apply_win_values(Drw* d, uint32_t mask, const uint8_t* v, const uint8_t* end) {
  for (int bit = 0; bit < 15; bit++) {
    if (!(mask & (1u << bit))) continue;
    uint32_t val = next_value(&v, end);
    switch (bit) {
      case 1:  d->bg_pixel = val & 0x00FFFFFFu; break;  // background-pixel
      case 3:  d->border_pixel = val & 0x00FFFFFFu; break;
      case 9:  d->override_redirect = (int)val; break;
      case 11: d->event_mask = val; break;
      case 13: d->colormap = val; break;
      default: break;
    }
  }
}

static void apply_gc_values(GCRec* g, uint32_t mask, const uint8_t* v, const uint8_t* end) {
  for (int bit = 0; bit < 23; bit++) {
    if (!(mask & (1u << bit))) continue;
    uint32_t val = next_value(&v, end);
    switch (bit) {
      case 0:  g->function = val; break;
      case 1:  g->plane_mask = val; break;
      case 2:  g->fg = val & 0x00FFFFFFu; break;
      case 3:  g->bg = val & 0x00FFFFFFu; break;
      case 4:  g->line_width = val; break;
      case 8:  g->fill_style = val; break;
      case 16: g->graphics_exposures = (int)val; break;
      case 22: g->arc_mode = val; break;
      default: break;
    }
  }
}

// ---------------------------------------------------------------------------
// core request dispatch
// ---------------------------------------------------------------------------
static const uint8_t kFallbackReply[256] = {
  // opcodes that MUST produce a reply; anything not handled explicitly gets a
  // zeroed 32-byte reply, which is a valid "empty" answer for all of them.
  [21] = 1,  [23] = 1,  [26] = 1,  [31] = 1,  [39] = 1,  [50] = 1,
  [52] = 1,  [83] = 1,  [85] = 1,  [86] = 1,  [87] = 1,  [92] = 1,
  [97] = 1,  [99] = 1,  [108] = 1, [110] = 1, [116] = 1, [118] = 1,
};

// --- keyboard mapping ------------------------------------------------------------------------
// GetKeyboardMapping used to return NoSymbol for everything, which is invisible until you try to
// TYPE: XLookupString then yields nothing and clients silently discard every key. This is a US
// layout indexed by EVDEV code (X keycode - 8), matching the codes window.m produces, with two
// symbols per key (unshifted, shifted).
#define XK_NONE 0x0000UL
static const uint32_t kKeysyms[128][2] = {
  [1]  = {0xFF1B, 0xFF1B},                                   // Escape
  [2]  = {'1','!'}, [3] = {'2','@'}, [4] = {'3','#'}, [5] = {'4','$'}, [6] = {'5','%'},
  [7]  = {'6','^'}, [8] = {'7','&'}, [9] = {'8','*'}, [10] = {'9','('}, [11] = {'0',')'},
  [12] = {'-','_'}, [13] = {'=','+'},
  [14] = {0xFF08, 0xFF08},                                   // BackSpace
  [15] = {0xFF09, 0xFE20},                                   // Tab / ISO_Left_Tab
  [16] = {'q','Q'}, [17] = {'w','W'}, [18] = {'e','E'}, [19] = {'r','R'}, [20] = {'t','T'},
  [21] = {'y','Y'}, [22] = {'u','U'}, [23] = {'i','I'}, [24] = {'o','O'}, [25] = {'p','P'},
  [26] = {'[','{'}, [27] = {']','}'},
  [28] = {0xFF0D, 0xFF0D},                                   // Return
  [29] = {0xFFE3, 0xFFE3},                                   // Control_L
  [30] = {'a','A'}, [31] = {'s','S'}, [32] = {'d','D'}, [33] = {'f','F'}, [34] = {'g','G'},
  [35] = {'h','H'}, [36] = {'j','J'}, [37] = {'k','K'}, [38] = {'l','L'},
  [39] = {';',':'}, [40] = {'\'','"'}, [41] = {'`','~'},
  [42] = {0xFFE1, 0xFFE1},                                   // Shift_L
  [43] = {'\\','|'},
  [44] = {'z','Z'}, [45] = {'x','X'}, [46] = {'c','C'}, [47] = {'v','V'}, [48] = {'b','B'},
  [49] = {'n','N'}, [50] = {'m','M'},
  [51] = {',','<'}, [52] = {'.','>'}, [53] = {'/','?'},
  [54] = {0xFFE2, 0xFFE2},                                   // Shift_R
  [56] = {0xFFE9, 0xFFE9},                                   // Alt_L
  [57] = {' ',' '},                                          // space
  [58] = {0xFFE5, 0xFFE5},                                   // Caps_Lock
  [59] = {0xFFBE,0xFFBE}, [60] = {0xFFBF,0xFFBF}, [61] = {0xFFC0,0xFFC0}, [62] = {0xFFC1,0xFFC1},
  [63] = {0xFFC2,0xFFC2}, [64] = {0xFFC3,0xFFC3}, [65] = {0xFFC4,0xFFC4}, [66] = {0xFFC5,0xFFC5},
  [67] = {0xFFC6,0xFFC6}, [68] = {0xFFC7,0xFFC7},
  [87] = {0xFFC8,0xFFC8}, [88] = {0xFFC9,0xFFC9},            // F11, F12
  [97] = {0xFFE4, 0xFFE4},                                   // Control_R
  [100]= {0xFFEA, 0xFFEA},                                   // Alt_R
  [102]= {0xFF50, 0xFF50},                                   // Home
  [103]= {0xFF52, 0xFF52},                                   // Up
  [104]= {0xFF55, 0xFF55},                                   // Page_Up
  [105]= {0xFF51, 0xFF51},                                   // Left
  [106]= {0xFF53, 0xFF53},                                   // Right
  [107]= {0xFF57, 0xFF57},                                   // End
  [108]= {0xFF54, 0xFF54},                                   // Down
  [109]= {0xFF56, 0xFF56},                                   // Page_Down
  [110]= {0xFF63, 0xFF63},                                   // Insert
  [111]= {0xFFFF, 0xFFFF},                                   // Delete
  [125]= {0xFFEB, 0xFFEB},                                   // Super_L
};

static void handle_request(uint8_t op, uint8_t detail, const uint8_t* d, size_t dlen) {
  uint8_t body[24];
  memset(body, 0, sizeof body);

  switch (op) {

  // ---- windows -------------------------------------------------------------
  case 1: {                                             // CreateWindow
    if (dlen < 28) break;
    uint32_t wid = g32(d), parent = g32(d + 4);
    Drw* w = d_new(wid);
    if (!w) { x_error(XERR_IDCHOICE, wid, op, 0); break; }
    w->is_window = 1;
    w->depth = detail ? detail : 24;
    w->x = gs16(d + 8); w->y = gs16(d + 10);
    w->w = g16(d + 12); w->h = g16(d + 14);
    w->border_width = g16(d + 16);
    w->class_ = g16(d + 18);
    if (w->class_ == 0) w->class_ = 1;                  // CopyFromParent -> InputOutput
    w->visual = g32(d + 20);
    w->parent = parent;
    w->bg_pixel = 0x00FFFFFF;
    w->colormap = ROOT_CMAP;
    uint32_t mask = g32(d + 24);
    apply_win_values(w, mask, d + 28, d + dlen);
    fb_alloc(w, w->bg_pixel);
    xlog("CreateWindow 0x%x parent=0x%x %dx%d+%d+%d depth=%u mask=0x%x evmask=0x%x",
         wid, parent, w->w, w->h, w->x, w->y, w->depth, mask, w->event_mask);
    break;
  }
  case 2: {                                             // ChangeWindowAttributes
    if (dlen < 8) break;
    Drw* w = d_find(g32(d));
    if (!w) { x_error(XERR_WINDOW, g32(d), op, 0); break; }
    uint32_t old_bg = w->bg_pixel, old_mask = w->event_mask;
    apply_win_values(w, g32(d + 4), d + 8, d + dlen);
    // If the client selects Exposure only after mapping, it would otherwise never
    // get its first paint — re-expose.
    if (w->mapped && !(old_mask & EV_EXPOSURE) && (w->event_mask & EV_EXPOSURE)) ev_expose(w);
    if (w->bg_pixel != old_bg && w->fb) {
      // repaint the background of an as-yet-unpainted window
      uint32_t v = rgba_of(w->bg_pixel);
      uint32_t oldv = rgba_of(old_bg);
      uint32_t* q = (uint32_t*)w->fb;
      size_t n = (size_t)w->w * w->h;
      for (size_t i = 0; i < n; i++) if (q[i] == oldv) q[i] = v;
      g_dirty = 1;
    }
    break;
  }
  case 3: {                                             // GetWindowAttributes
    if (dlen < 4) break;
    Drw* w = d_find(g32(d));
    if (!w) { x_error(XERR_WINDOW, g32(d), op, 0); break; }
    uint8_t tail[12];
    memset(tail, 0, sizeof tail);
    p32(body + 0, w->visual ? w->visual : VISUAL_24);   // 8  visual
    p16(body + 4, w->class_);                           // 12 class
    body[6] = 0;                                        // 14 bit-gravity
    body[7] = 1;                                        // 15 win-gravity
    p32(body + 8, 0);                                   // 16 backing-planes
    p32(body + 12, 0);                                  // 20 backing-pixel
    body[16] = 0;                                       // 24 save-under
    body[17] = 1;                                       // 25 map-is-installed
    body[18] = (uint8_t)(w->mapped ? 2 : 0);            // 26 map-state
    body[19] = (uint8_t)w->override_redirect;           // 27 override-redirect
    p32(body + 20, w->colormap);                        // 28 colormap
    p32(tail + 0, w->event_mask);                       // 32 all-event-masks
    p32(tail + 4, w->event_mask);                       // 36 your-event-mask
    p16(tail + 8, 0);                                   // 40 do-not-propagate
    reply(0 /*backing-store: Never*/, body, tail, sizeof tail);
    break;
  }
  case 4: case 5: {                                     // DestroyWindow / DestroySubwindows
    if (dlen < 4) break;
    uint32_t id = g32(d);
    for (int i = 0; i < MAX_DRAWABLES; i++)
      if (g_drw[i].used && g_drw[i].parent == id) d_free(&g_drw[i]);
    if (op == 4) { Drw* w = d_find(id); if (w) d_free(w); if (g_present_win == id) g_present_win = 0; }
    g_dirty = 1;
    break;
  }
  case 8: {                                             // MapWindow
    if (dlen < 4) break;
    Drw* w = d_find(g32(d));
    if (!w) { x_error(XERR_WINDOW, g32(d), op, 0); break; }
    emit_map_events(w);
    xlog("MapWindow 0x%x", w->id);
    break;
  }
  case 9: {                                             // MapSubwindows  (MANDATORY: xeyes)
    if (dlen < 4) break;
    uint32_t id = g32(d);
    int n = 0;
    for (unsigned pass = 1; pass <= g_drw_seq; pass++)
      for (int i = 0; i < MAX_DRAWABLES; i++) {
        Drw* c = &g_drw[i];
        if (c->used && c->is_window && c->parent == id && c->seq == pass) { emit_map_events(c); n++; }
      }
    xlog("MapSubwindows 0x%x -> %d children", id, n);
    break;
  }
  case 10: {                                            // UnmapWindow
    if (dlen < 4) break;
    Drw* w = d_find(g32(d));
    if (!w) break;
    if (w->mapped) { w->mapped = 0; ev_unmap(w); g_dirty = 1; }
    break;
  }
  case 11:                                              // UnmapSubwindows
    if (dlen >= 4) {
      uint32_t id = g32(d);
      for (int i = 0; i < MAX_DRAWABLES; i++)
        if (g_drw[i].used && g_drw[i].parent == id && g_drw[i].mapped) {
          g_drw[i].mapped = 0; ev_unmap(&g_drw[i]);
        }
      g_dirty = 1;
    }
    break;
  case 12: {                                            // ConfigureWindow
    if (dlen < 8) break;
    Drw* w = d_find(g32(d));
    if (!w) { x_error(XERR_WINDOW, g32(d), op, 0); break; }
    uint16_t mask = g16(d + 4);
    const uint8_t* v = d + 8; const uint8_t* end = d + dlen;
    uint32_t nw = w->w, nh = w->h;
    for (int bit = 0; bit < 7; bit++) {
      if (!(mask & (1u << bit))) continue;
      uint32_t val = next_value(&v, end);
      switch (bit) {
        case 0: w->x = (int16_t)(val & 0xFFFF); break;
        case 1: w->y = (int16_t)(val & 0xFFFF); break;
        case 2: nw = val & 0xFFFF; break;
        case 3: nh = val & 0xFFFF; break;
        case 4: w->border_width = (uint16_t)val; break;
        default: break;
      }
    }
    if ((nw != w->w || nh != w->h) && nw && nh) {
      uint32_t ow = w->w, oh = w->h;
      uint8_t* old = w->fb;
      w->w = nw; w->h = nh; w->fb = NULL;
      fb_alloc(w, w->bg_pixel);
      if (old && w->fb) {
        uint32_t cw = ow < nw ? ow : nw, ch = oh < nh ? oh : nh;
        for (uint32_t y = 0; y < ch; y++)
          memcpy(w->fb + (size_t)y * nw * 4, old + (size_t)y * ow * 4, (size_t)cw * 4);
      }
      free(old);
      ev_expose(w);
    }
    ev_configure(w);
    g_dirty = 1;
    xlog("ConfigureWindow 0x%x -> %ux%u+%d+%d", w->id, w->w, w->h, w->x, w->y);
    break;
  }
  case 14: {                                            // GetGeometry
    if (dlen < 4) break;
    uint32_t id = g32(d);
    Drw* w = d_find(id);
    uint8_t depth = 24;
    int32_t x = 0, y = 0; uint32_t ww = SCREEN_W, hh = SCREEN_H; uint16_t bw = 0;
    if (w) { depth = w->depth ? w->depth : 24; x = w->x; y = w->y; ww = w->w; hh = w->h; bw = w->border_width; }
    else if (id != ROOT_WIN) { x_error(XERR_DRAWABLE, id, op, 0); break; }
    p32(body + 0, ROOT_WIN);
    p16(body + 4, (uint16_t)x); p16(body + 6, (uint16_t)y);
    p16(body + 8, (uint16_t)ww); p16(body + 10, (uint16_t)hh);
    p16(body + 12, bw);
    reply(depth, body, NULL, 0);
    break;
  }
  case 15: {                                            // QueryTree
    if (dlen < 4) break;
    uint32_t id = g32(d);
    Drw* w = d_find(id);
    uint32_t kids[MAX_DRAWABLES]; int n = 0;
    for (unsigned pass = 1; pass <= g_drw_seq; pass++)
      for (int i = 0; i < MAX_DRAWABLES; i++)
        if (g_drw[i].used && g_drw[i].is_window && g_drw[i].parent == id && g_drw[i].seq == pass)
          kids[n++] = g_drw[i].id;
    p32(body + 0, ROOT_WIN);
    p32(body + 4, w ? w->parent : 0);
    p16(body + 8, (uint16_t)n);
    uint8_t* tail = (uint8_t*)malloc((size_t)n * 4 + 4);
    if (!tail) break;
    for (int i = 0; i < n; i++) p32(tail + i * 4, kids[i]);
    reply(0, body, tail, (size_t)n * 4);
    free(tail);
    break;
  }

  // ---- atoms / properties --------------------------------------------------
  case 16: {                                            // InternAtom
    if (dlen < 4) break;
    uint16_t n = g16(d);
    if (dlen < 4u + n) break;
    uint32_t a = atom_intern((const char*)d + 4, n, detail);
    p32(body + 0, a);
    reply(0, body, NULL, 0);
    xlog("InternAtom \"%.*s\" -> %u", (int)n, (const char*)d + 4, a);
    break;
  }
  case 17: {                                            // GetAtomName
    if (dlen < 4) break;
    const char* s = atom_name(g32(d));
    if (!s) { x_error(XERR_ATOM, g32(d), op, 0); break; }
    uint16_t n = (uint16_t)strlen(s);
    p16(body + 0, n);
    reply(0, body, s, n);
    break;
  }
  case 18: {                                            // ChangeProperty
    if (dlen < 20) break;
    uint32_t win = g32(d), prop = g32(d + 4), type = g32(d + 8);
    uint8_t fmt = d[12];
    uint32_t nunits = g32(d + 16);
    uint32_t nbytes = nunits * (fmt / 8 ? fmt / 8 : 1);
    if (20u + nbytes > dlen) nbytes = (uint32_t)(dlen > 20 ? dlen - 20 : 0);
    prop_set(win, prop, type, fmt, d + 20, nbytes, detail);
    break;
  }
  case 19:                                              // DeleteProperty
    if (dlen >= 8) prop_delete(g32(d), g32(d + 4));
    break;
  case 20: {                                            // GetProperty
    if (dlen < 20) break;
    uint32_t win = g32(d), prop = g32(d + 4), type = g32(d + 8);
    uint32_t loff = g32(d + 12), llen = g32(d + 16);
    Prop* p = prop_find(win, prop);
    if (!p || (type != 0 && p->type != type)) {
      p32(body + 0, p ? p->type : 0);
      p32(body + 4, 0); p32(body + 8, 0);
      reply(0, body, NULL, 0);
      break;
    }
    uint32_t start = loff * 4;
    if (start > p->len) start = p->len;
    uint32_t avail = p->len - start;
    uint32_t want = llen > 0x3FFFFFFFu ? avail : llen * 4;
    uint32_t n = avail < want ? avail : want;
    uint32_t after = avail - n;
    uint8_t fmt = p->format ? p->format : 8;
    p32(body + 0, p->type);
    p32(body + 4, after);
    p32(body + 8, n / (fmt / 8 ? fmt / 8 : 1));
    reply(fmt, body, p->data + start, n);
    if (detail && after == 0) prop_delete(win, prop);
    break;
  }

  // ---- pointer / focus -----------------------------------------------------
  case 38: {                                            // QueryPointer (MANDATORY: xeyes)
    // x11srv_pointer() is LOCAL to the Metal window, i.e. to the presented
    // top-level window — so root coords are that window's origin plus it.
    int px = 0, py = 0;
    x11srv_pointer(&px, &py);
    Drw* top = pick_present_window();
    for (Drw* c = top; c && c->id != ROOT_WIN; c = d_find(c->parent)) { px += c->x; py += c->y; }
    uint32_t win = dlen >= 4 ? g32(d) : ROOT_WIN;
    Drw* w = d_find(win);
    int wx = px, wy = py;
    if (w) {                                            // translate to window-local
      int ox = 0, oy = 0;
      for (Drw* c = w; c && c->id != ROOT_WIN; c = d_find(c->parent)) { ox += c->x; oy += c->y; }
      wx = px - ox; wy = py - oy;
    }
    p32(body + 0, ROOT_WIN);                            // root
    p32(body + 4, 0);                                   // child
    p16(body + 8, (uint16_t)px); p16(body + 10, (uint16_t)py);
    p16(body + 12, (uint16_t)wx); p16(body + 14, (uint16_t)wy);
    p16(body + 16, 0);                                  // key/button mask
    reply(1 /*same-screen = True*/, body, NULL, 0);
    break;
  }
  case 40: {                                            // TranslateCoordinates
    if (dlen < 12) break;
    Drw* src = d_find(g32(d));
    Drw* dst = d_find(g32(d + 4));
    int sx = gs16(d + 8), sy = gs16(d + 10);
    int ax = sx, ay = sy;
    for (Drw* c = src; c && c->id != ROOT_WIN; c = d_find(c->parent)) { ax += c->x; ay += c->y; }
    for (Drw* c = dst; c && c->id != ROOT_WIN; c = d_find(c->parent)) { ax -= c->x; ay -= c->y; }
    p32(body + 0, 0);                                   // child = None
    p16(body + 4, (uint16_t)ax); p16(body + 6, (uint16_t)ay);
    reply(1, body, NULL, 0);
    break;
  }
  case 43: {                                            // GetInputFocus
    p32(body + 0, g_present_win ? g_present_win : ROOT_WIN);
    reply(1 /*revert-to PointerRoot*/, body, NULL, 0);
    break;
  }
  case 44: {                                            // QueryKeymap
    uint8_t tail[32]; memset(tail, 0, sizeof tail);
    reply(0, body, tail, sizeof tail);
    break;
  }

  // ---- pixmaps / GCs -------------------------------------------------------
  case 53: {                                            // CreatePixmap
    if (dlen < 12) break;
    uint32_t pid = g32(d);
    Drw* p = d_new(pid);
    if (!p) { x_error(XERR_IDCHOICE, pid, op, 0); break; }
    p->is_window = 0;
    p->depth = detail ? detail : 24;
    p->w = g16(d + 8); p->h = g16(d + 10);
    p->class_ = 1;
    fb_alloc(p, 0x000000);
    break;
  }
  case 54:                                              // FreePixmap
    if (dlen >= 4) { Drw* p = d_find(g32(d)); if (p) d_free(p); }
    break;
  case 55: {                                            // CreateGC
    if (dlen < 12) break;
    uint32_t cid = g32(d);
    GCRec* g = gc_new(cid);
    if (!g) { x_error(XERR_IDCHOICE, cid, op, 0); break; }
    g->drawable = g32(d + 4);
    apply_gc_values(g, g32(d + 8), d + 12, d + dlen);
    break;
  }
  case 56: {                                            // ChangeGC
    if (dlen < 8) break;
    GCRec* g = gc_find(g32(d));
    if (!g) { x_error(XERR_GC, g32(d), op, 0); break; }
    apply_gc_values(g, g32(d + 4), d + 8, d + dlen);
    break;
  }
  case 57: {                                            // CopyGC
    if (dlen < 12) break;
    GCRec* s = gc_find(g32(d)); GCRec* t = gc_find(g32(d + 4));
    if (s && t) { uint32_t id = t->id; *t = *s; t->id = id; }
    break;
  }
  case 58: case 59: break;                              // SetDashes / SetClipRectangles
  case 60:                                              // FreeGC
    if (dlen >= 4) { GCRec* g = gc_find(g32(d)); if (g) memset(g, 0, sizeof *g); }
    break;

  // ---- drawing -------------------------------------------------------------
  case 61: {                                            // ClearArea
    if (dlen < 12) break;
    Drw* w = d_find(g32(d));
    if (!w) { x_error(XERR_WINDOW, g32(d), op, 0); break; }
    int x = gs16(d + 4), y = gs16(d + 6);
    int cw = g16(d + 8), ch = g16(d + 10);
    if (!cw) cw = (int)w->w - x;
    if (!ch) ch = (int)w->h - y;
    fill_rect(w, NULL, x, y, cw, ch, rgba_of(w->bg_pixel));
    if (detail) ev_expose(w);
    g_dirty = 1;
    break;
  }
  case 62: {                                            // CopyArea
    if (dlen < 24) break;
    Drw* s = d_find(g32(d)); Drw* t = d_find(g32(d + 4));
    GCRec* g = gc_find(g32(d + 8));
    if (!s || !t || !s->fb || !t->fb) break;
    int sx = gs16(d + 12), sy = gs16(d + 14), dx = gs16(d + 16), dy = gs16(d + 18);
    int cw = g16(d + 20), ch = g16(d + 22);
    for (int y = 0; y < ch; y++)
      for (int x = 0; x < cw; x++) {
        if (sx + x < 0 || sy + y < 0 || sx + x >= (int)s->w || sy + y >= (int)s->h) continue;
        px_op(t, g, dx + x, dy + y, px_get(s, sx + x, sy + y));
      }
    g_dirty = 1;
    break;
  }
  case 64: {                                            // PolyPoint
    if (dlen < 8) break;
    Drw* t = d_find(g32(d)); GCRec* g = gc_find(g32(d + 4));
    if (!t || !g) break;
    uint32_t c = rgba_of(g->fg);
    int x = 0, y = 0;
    for (size_t o = 8; o + 4 <= dlen; o += 4) {
      int nx = gs16(d + o), ny = gs16(d + o + 2);
      if (detail == 1 && o > 8) { x += nx; y += ny; } else { x = nx; y = ny; }
      px_op(t, g, x, y, c);
    }
    g_dirty = 1;
    break;
  }
  case 65: {                                            // PolyLine
    if (dlen < 8) break;
    Drw* t = d_find(g32(d)); GCRec* g = gc_find(g32(d + 4));
    if (!t || !g) break;
    uint32_t c = rgba_of(g->fg);
    int px_ = 0, py_ = 0, first = 1;
    for (size_t o = 8; o + 4 <= dlen; o += 4) {
      int nx = gs16(d + o), ny = gs16(d + o + 2);
      int x, y;
      if (detail == 1 && !first) { x = px_ + nx; y = py_ + ny; } else { x = nx; y = ny; }
      if (!first) draw_line(t, g, px_, py_, x, y, c);
      px_ = x; py_ = y; first = 0;
    }
    g_dirty = 1;
    break;
  }
  case 66: {                                            // PolySegment
    if (dlen < 8) break;
    Drw* t = d_find(g32(d)); GCRec* g = gc_find(g32(d + 4));
    if (!t || !g) break;
    uint32_t c = rgba_of(g->fg);
    for (size_t o = 8; o + 8 <= dlen; o += 8)
      draw_line(t, g, gs16(d + o), gs16(d + o + 2), gs16(d + o + 4), gs16(d + o + 6), c);
    g_dirty = 1;
    break;
  }
  case 67: {                                            // PolyRectangle
    if (dlen < 8) break;
    Drw* t = d_find(g32(d)); GCRec* g = gc_find(g32(d + 4));
    if (!t || !g) break;
    uint32_t c = rgba_of(g->fg);
    for (size_t o = 8; o + 8 <= dlen; o += 8) {
      int x = gs16(d + o), y = gs16(d + o + 2), w = g16(d + o + 4), h = g16(d + o + 6);
      draw_line(t, g, x, y, x + w, y, c);
      draw_line(t, g, x + w, y, x + w, y + h, c);
      draw_line(t, g, x + w, y + h, x, y + h, c);
      draw_line(t, g, x, y + h, x, y, c);
    }
    g_dirty = 1;
    break;
  }
  case 68: {                                            // PolyArc
    if (dlen < 8) break;
    Drw* t = d_find(g32(d)); GCRec* g = gc_find(g32(d + 4));
    if (!t || !g) break;
    uint32_t c = rgba_of(g->fg);
    for (size_t o = 8; o + 12 <= dlen; o += 12)
      stroke_arc(t, g, gs16(d + o), gs16(d + o + 2), g16(d + o + 4), g16(d + o + 6),
                 gs16(d + o + 8), gs16(d + o + 10), c);
    g_dirty = 1;
    break;
  }
  case 69: {                                            // FillPoly
    if (dlen < 12) break;
    Drw* t = d_find(g32(d)); GCRec* g = gc_find(g32(d + 4));
    if (!t || !g) break;
    uint8_t coord_mode = d[9];
    int n = (int)((dlen - 12) / 4);
    if (n < 3) break;
    int32_t* pts = (int32_t*)malloc(sizeof(int32_t) * 2 * (size_t)n);
    if (!pts) break;
    int x = 0, y = 0;
    for (int i = 0; i < n; i++) {
      int nx = gs16(d + 12 + i * 4), ny = gs16(d + 12 + i * 4 + 2);
      if (coord_mode == 1 && i > 0) { x += nx; y += ny; } else { x = nx; y = ny; }
      pts[i * 2] = x; pts[i * 2 + 1] = y;
    }
    fill_poly(t, g, pts, n, rgba_of(g->fg));
    free(pts);
    g_dirty = 1;
    break;
  }
  case 70: {                                            // PolyFillRectangle
    if (dlen < 8) break;
    Drw* t = d_find(g32(d)); GCRec* g = gc_find(g32(d + 4));
    if (!t || !g) break;
    uint32_t c = rgba_of(g->fg);
    for (size_t o = 8; o + 8 <= dlen; o += 8)
      fill_rect(t, g, gs16(d + o), gs16(d + o + 2), g16(d + o + 4), g16(d + o + 6), c);
    g_dirty = 1;
    break;
  }
  case 71: {                                            // PolyFillArc (MANDATORY: xeyes)
    if (dlen < 8) break;
    Drw* t = d_find(g32(d)); GCRec* g = gc_find(g32(d + 4));
    if (!t || !g) break;
    uint32_t c = rgba_of(g->fg);
    int n = 0;
    for (size_t o = 8; o + 12 <= dlen; o += 12) {
      fill_arc(t, g, gs16(d + o), gs16(d + o + 2), g16(d + o + 4), g16(d + o + 6),
               gs16(d + o + 8), gs16(d + o + 10), c);
      n++;
    }
    g_dirty = 1;
    xlog("PolyFillArc 0x%x n=%d fg=0x%06x", t->id, n, g->fg);
    break;
  }
  case 72: {                                            // PutImage
    if (dlen < 16) break;
    Drw* t = d_find(g32(d)); GCRec* g = gc_find(g32(d + 4));
    if (!t || !t->fb) break;
    int iw = g16(d + 8), ih = g16(d + 10);
    int dx = gs16(d + 12), dy = gs16(d + 14);
    uint8_t left_pad = d[16], depth = d[17];
    const uint8_t* img = d + 20;
    size_t avail = dlen > 20 ? dlen - 20 : 0;
    if (detail == 2) {                                  // ZPixmap
      int bpp = (depth == 1) ? 1 : 32;
      if (bpp == 32) {
        size_t stride = PAD4((size_t)iw * 4);
        for (int y = 0; y < ih; y++) {
          if ((size_t)(y + 1) * stride > avail) break;
          const uint8_t* row = img + (size_t)y * stride;
          for (int x = 0; x < iw; x++) {
            uint32_t v = g32(row + x * 4);
            px_set(t, dx + x, dy + y, rgba_of(v));
          }
        }
      }
    } else {                                            // Bitmap(0) / XYPixmap(1), depth 1
      size_t stride = PAD4((size_t)(iw + left_pad + 7) / 8);
      uint32_t fg = rgba_of(g ? g->fg : 0), bg = rgba_of(g ? g->bg : 0xFFFFFF);
      for (int y = 0; y < ih; y++) {
        if ((size_t)(y + 1) * stride > avail) break;
        const uint8_t* row = img + (size_t)y * stride;
        for (int x = 0; x < iw; x++) {
          int b = x + left_pad;
          int on = (row[b >> 3] >> (b & 7)) & 1;        // LSBFirst bit order
          px_set(t, dx + x, dy + y, on ? fg : bg);
        }
      }
    }
    g_dirty = 1;
    break;
  }
  case 73: {                                            // GetImage
    if (dlen < 16) break;
    Drw* s = d_find(g32(d));
    if (!s || !s->fb) { x_error(XERR_DRAWABLE, dlen >= 4 ? g32(d) : 0, op, 0); break; }
    int x = gs16(d + 4), y = gs16(d + 6);
    int w = g16(d + 8), h = g16(d + 10);
    size_t stride = PAD4((size_t)w * 4);
    size_t n = stride * (size_t)h;
    uint8_t* tail = (uint8_t*)calloc(1, n ? n : 4);
    if (!tail) break;
    for (int yy = 0; yy < h; yy++)
      for (int xx = 0; xx < w; xx++)
        p32(tail + (size_t)yy * stride + (size_t)xx * 4, pixel_of(px_get(s, x + xx, y + yy)));
    p32(body + 0, s->depth == 32 ? VISUAL_32 : VISUAL_24);
    reply(s->depth ? s->depth : 24, body, tail, n);
    free(tail);
    break;
  }
  case 74: {                                            // PolyText8
    if (dlen < 12) break;
    Drw* t = d_find(g32(d)); GCRec* g = gc_find(g32(d + 4));
    if (!t || !g) break;
    int x = gs16(d + 8), y = gs16(d + 10);
    size_t o = 12;
    while (o + 1 < dlen) {
      uint8_t n = d[o];
      if (n == 255) { o += 5; continue; }               // font shift
      if (n == 0) break;
      if (o + 2 + n > dlen) break;
      int8_t delta = (int8_t)d[o + 1];
      x += delta;
      draw_text(t, g, x, y, d + o + 2, n, 0);
      x += n * FONT_W;
      o += 2 + n;
    }
    g_dirty = 1;
    break;
  }
  case 76: {                                            // ImageText8
    if (dlen < 12) break;
    Drw* t = d_find(g32(d)); GCRec* g = gc_find(g32(d + 4));
    if (!t || !g) break;
    int x = gs16(d + 8), y = gs16(d + 10);
    int n = detail;
    if (12u + (size_t)n > dlen) n = (int)(dlen - 12);
    draw_text(t, g, x, y, d + 12, n, 1);
    g_dirty = 1;
    break;
  }

  // ---- colours -------------------------------------------------------------
  case 78:                                              // CreateColormap
    if (dlen >= 12 && g_ncmaps < MAX_CMAPS) g_cmaps[g_ncmaps++] = g32(d);
    break;
  case 79: case 80: case 81: case 82: break;            // Free/Copy/Install/UninstallColormap
  case 84: {                                            // AllocColor
    if (dlen < 12) break;
    uint16_t r = g16(d + 4), gg = g16(d + 6), b = g16(d + 8);
    uint32_t pixel = ((uint32_t)(r >> 8) << 16) | ((uint32_t)(gg >> 8) << 8) | (uint32_t)(b >> 8);
    p16(body + 0, (uint16_t)((r >> 8) * 257));
    p16(body + 2, (uint16_t)((gg >> 8) * 257));
    p16(body + 4, (uint16_t)((b >> 8) * 257));
    p32(body + 8, pixel);
    reply(0, body, NULL, 0);
    break;
  }
  case 88: break;                                       // FreeColors
  case 91: {                                            // QueryColors (MANDATORY: libXt)
    if (dlen < 4) break;
    int n = (int)((dlen - 4) / 4);
    uint8_t* tail = (uint8_t*)calloc(1, (size_t)(n ? n : 1) * 8);
    if (!tail) break;
    for (int i = 0; i < n; i++) {
      uint32_t px = g32(d + 4 + i * 4);
      uint32_t r = (px >> 16) & 0xFF, gg = (px >> 8) & 0xFF, b = px & 0xFF;
      p16(tail + i * 8 + 0, (uint16_t)(r * 257));
      p16(tail + i * 8 + 2, (uint16_t)(gg * 257));
      p16(tail + i * 8 + 4, (uint16_t)(b * 257));
      p16(tail + i * 8 + 6, 0);
    }
    p16(body + 0, (uint16_t)n);                         // numRGBs at reply offset 8
    reply(0, body, tail, (size_t)n * 8);
    free(tail);
    xlog("QueryColors n=%d", n);
    break;
  }

  // ---- fonts ---------------------------------------------------------------
  case 45: case 46: break;                              // OpenFont / CloseFont
  case 47: {                                            // QueryFont
    const int mmin = 32, mmax = 126;
    const int m = mmax - mmin + 1;
    size_t tail_len = 28 + (size_t)m * 12;
    uint8_t* tail = (uint8_t*)calloc(1, tail_len);
    if (!tail) break;
    // body covers reply offsets 8..31
    // min-bounds CHARINFO at 8..19, pad 20..23, max-bounds 24..31 (spills into tail)
    uint8_t ci[12];
    memset(ci, 0, sizeof ci);
    p16(ci + 0, 0);                                     // left side bearing
    p16(ci + 2, FONT_W);                                // right side bearing
    p16(ci + 4, FONT_W);                                // character width
    p16(ci + 6, FONT_ASCENT);
    p16(ci + 8, FONT_DESCENT);
    p16(ci + 10, 0);
    memcpy(body + 0, ci, 12);                           // min-bounds  (offset 8)
    memcpy(body + 16, ci, 8);                           // max-bounds first 8 (offset 24)
    memcpy(tail + 0, ci + 8, 4);                        // max-bounds rest    (offset 32)
    // tail offset 4..7 = pad after max-bounds  (reply offsets 36..39)
    p16(tail + 8, (uint16_t)mmin);                      // 40 min-char-or-byte2
    p16(tail + 10, (uint16_t)mmax);                     // 42 max-char-or-byte2
    p16(tail + 12, 32);                                 // 44 default-char
    p16(tail + 14, 0);                                  // 46 number of FONTPROPs
    tail[16] = 0;                                       // 48 draw-direction LtoR
    tail[17] = 0;                                       // 49 min-byte1
    tail[18] = 0;                                       // 50 max-byte1
    tail[19] = 1;                                       // 51 all-chars-exist
    p16(tail + 20, FONT_ASCENT);                        // 52 font-ascent
    p16(tail + 22, FONT_DESCENT);                       // 54 font-descent
    p32(tail + 24, (uint32_t)m);                        // 56 number of CHARINFOs
    for (int i = 0; i < m; i++) memcpy(tail + 28 + (size_t)i * 12, ci, 12);
    reply(0, body, tail, tail_len);
    free(tail);
    break;
  }
  case 48: {                                            // QueryTextExtents
    size_t nchars = dlen > 8 ? (dlen - 8) / 2 : 0;
    p16(body + 0, FONT_ASCENT);                         // font-ascent
    p16(body + 2, FONT_DESCENT);                        // font-descent
    p16(body + 4, FONT_ASCENT);                         // overall-ascent
    p16(body + 6, FONT_DESCENT);                        // overall-descent
    p32(body + 8, (uint32_t)(nchars * FONT_W));         // overall-width
    p32(body + 12, 0);                                  // overall-left
    p32(body + 16, (uint32_t)(nchars * FONT_W));        // overall-right
    reply(0 /*LtoR*/, body, NULL, 0);
    break;
  }
  case 49: {                                            // ListFonts
    static const char kFixed[] = "fixed";
    uint8_t tail[8];
    memset(tail, 0, sizeof tail);
    tail[0] = (uint8_t)(sizeof(kFixed) - 1);
    memcpy(tail + 1, kFixed, sizeof(kFixed) - 1);
    p16(body + 0, 1);                                   // number of names
    reply(0, body, tail, 6);
    break;
  }
  case 51: break;                                       // SetFontPath

  // ---- keyboard / misc -----------------------------------------------------
  case 101: {                                           // GetKeyboardMapping
    uint8_t first = dlen >= 1 ? d[0] : 8;
    uint8_t count = dlen >= 2 ? d[1] : 1;
    if (!count) count = 1;
    const uint8_t nsyms = 2;                            // unshifted, shifted
    size_t n = (size_t)count * nsyms * 4;
    uint8_t* tail = (uint8_t*)calloc(1, n ? n : 4);
    if (!tail) break;
    for (unsigned i = 0; i < count; i++) {
      unsigned kc = (unsigned)first + i;                // X keycode
      unsigned ev = kc >= 8 ? kc - 8 : 0;               // evdev code
      uint32_t s0 = ev < 128 ? kKeysyms[ev][0] : 0;
      uint32_t s1 = ev < 128 ? kKeysyms[ev][1] : 0;
      p32(tail + (i * nsyms + 0) * 4, s0);
      p32(tail + (i * nsyms + 1) * 4, s1);
    }
    reply(nsyms, body, tail, n);
    free(tail);
    break;
  }
  case 103: {                                           // GetKeyboardControl
    uint8_t tail[20];
    memset(tail, 0, sizeof tail);
    p32(body + 0, 0);                                   // led mask
    body[4] = 50;                                       // key-click-percent
    body[5] = 50;                                       // bell-percent
    p16(body + 6, 400);                                 // bell-pitch
    p16(body + 8, 100);                                 // bell-duration
    reply(1 /*global-auto-repeat On*/, body, tail, sizeof tail);
    break;
  }
  case 104:                                             // Bell
    xlog("Bell %d", (int8_t)detail);
    break;
  case 106: {                                           // GetPointerControl
    p16(body + 0, 2); p16(body + 2, 1); p16(body + 4, 4);
    reply(0, body, NULL, 0);
    break;
  }
  case 97: {                                            // QueryBestSize
    p16(body + 0, dlen >= 8 ? g16(d + 4) : 16);
    p16(body + 2, dlen >= 8 ? g16(d + 6) : 16);
    reply(0, body, NULL, 0);
    break;
  }
  case 117: {                                           // GetPointerMapping
    uint8_t tail[3] = { 1, 2, 3 };
    reply(3, body, tail, sizeof tail);
    break;
  }
  case 119: {                                           // GetModifierMapping
    // 8 modifier slots x keycodes-per-modifier. Without Shift/Control/Alt listed here a client
    // cannot resolve the shifted level of a key, so typing capitals and symbols breaks.
    {
      const uint8_t kpm = 2;                            // keycodes per modifier
      uint8_t map[8 * 2]; memset(map, 0, sizeof map);
      map[0 * kpm + 0] = 42 + 8;                        // Shift:   Shift_L
      map[0 * kpm + 1] = 54 + 8;                        //          Shift_R
      map[1 * kpm + 0] = 58 + 8;                        // Lock:    Caps_Lock
      map[2 * kpm + 0] = 29 + 8;                        // Control: Control_L
      map[2 * kpm + 1] = 97 + 8;                        //          Control_R
      map[3 * kpm + 0] = 56 + 8;                        // Mod1:    Alt_L
      map[3 * kpm + 1] = 100 + 8;                       //          Alt_R
      map[6 * kpm + 0] = 125 + 8;                       // Mod4:    Super_L
      reply(kpm, body, map, sizeof map);
      break;
    }
    const uint8_t n = 2;
    uint8_t tail[8 * 2];
    memset(tail, 0, sizeof tail);
    reply(n, body, tail, sizeof tail);
    break;
  }

  // ---- extensions ----------------------------------------------------------
  case 98: {                                            // QueryExtension
    if (dlen < 4) break;
    uint16_t n = g16(d);
    if (dlen < 4u + n) break;
    const char* name = (const char*)d + 4;
    int present = 0; uint8_t major = 0;
    if (n == 12 && !memcmp(name, "BIG-REQUESTS", 12)) { present = 1; major = BIGREQ_MAJOR; }
    else if (n == 3 && !memcmp(name, "GLX", 3))       { present = 1; major = GLX_MAJOR; }
    else if (n == 15 && !memcmp(name, "XInputExtension", 15)) { present = 1; major = XI_MAJOR; }
    body[0] = (uint8_t)present;                         // present
    body[1] = major;                                    // major-opcode
    body[2] = major == XI_MAJOR ? XI_FIRST_EV  : 0;     // first-event
    body[3] = major == XI_MAJOR ? XI_FIRST_ERR : 0;     // first-error
    reply(0, body, NULL, 0);
    xlog("QueryExtension \"%.*s\" -> present=%d major=%u", (int)n, name, present, major);
    break;
  }
  case 99: {                                            // ListExtensions
    static const char* kExt[2] = { "BIG-REQUESTS", "GLX" };
    uint8_t tail[32]; size_t o = 0;
    for (int i = 0; i < 2; i++) {
      size_t l = strlen(kExt[i]);
      tail[o++] = (uint8_t)l;
      memcpy(tail + o, kExt[i], l); o += l;
    }
    reply(2 /*number of STRs, in the detail byte*/, body, tail, o);
    break;
  }
  case BIGREQ_MAJOR: {                                  // BIG-REQUESTS
    if (detail == 0) {                                  // BigReqEnable
      g_bigreq = 1;
      p32(body + 0, 4194303);                           // maximum-request-length (units)
      reply(0, body, NULL, 0);
      xlog("BIG-REQUESTS enabled");
    }
    break;
  }
  case GLX_MAJOR:
    handle_glx(detail, d, dlen);
    break;
  case XI_MAJOR:
    handle_xinput(detail, d, dlen);
    break;

  // ---- no-reply requests we simply accept ----------------------------------
  case 6: case 7: case 13: case 22: case 24: case 25: case 27: case 28: case 29:
  case 30: case 32: case 33: case 34: case 35: case 36: case 37: case 41: case 42:
  case 63: case 75: case 77: case 89: case 90: case 93: case 94: case 95: case 96:
  case 100: case 102: case 105: case 107: case 109: case 111: case 112: case 113:
  case 114: case 115: case 127:
    break;

  default:
    if (kFallbackReply[op]) {
      reply(0, body, NULL, 0);
      xwarn("opcode %u unhandled — sent empty reply", op);
    } else {
      xwarn("opcode %u unhandled (no reply expected)", op);
    }
    break;
  }
}

// ---------------------------------------------------------------------------
// client loop
// ---------------------------------------------------------------------------
static uint8_t* g_ib; static size_t g_ib_len, g_ib_cap;

static int ib_fill(void) {
  if (g_ib_len + 65536 > g_ib_cap) {
    size_t cap = g_ib_cap ? g_ib_cap * 2 : 262144;
    while (cap < g_ib_len + 65536) cap *= 2;
    uint8_t* nb = (uint8_t*)realloc(g_ib, cap);
    if (!nb) return -1;
    g_ib = nb; g_ib_cap = cap;
  }
  ssize_t k = read(g_cfd, g_ib + g_ib_len, g_ib_cap - g_ib_len);
  if (k > 0) { g_ib_len += (size_t)k; return 1; }
  if (k == 0) return 0;                                 // EOF
  if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 2;
  return -1;
}
static void ib_consume(size_t n) {
  if (n >= g_ib_len) { g_ib_len = 0; return; }
  memmove(g_ib, g_ib + n, g_ib_len - n);
  g_ib_len -= n;
}

// Returns 0 to keep going, -1 on protocol failure.
static int do_setup(void) {
  // wait for the 12-byte prologue then the auth strings
  while (g_ib_len < 12) {
    int r = ib_fill();
    if (r == 0 || r < 0) return -1;
  }
  uint8_t order = g_ib[0];
  uint16_t maj = g16(g_ib + 2), min = g16(g_ib + 4);
  uint16_t n = g16(g_ib + 6), dd = g16(g_ib + 8);
  size_t need = 12 + PAD4(n) + PAD4(dd);
  while (g_ib_len < need) {
    int r = ib_fill();
    if (r == 0 || r < 0) return -1;
  }
  if (order != 'l')
    xwarn("client byte order '%c' (0x%02x) — only LSBFirst is supported", order, order);
  glxd_set_byte_order(order == 'l');                    // the GL stream follows the same order
  xlog("setup request: order='%c' proto=%u.%u auth=%u/%u", order, maj, min, n, dd);
  ib_consume(need);
  send_setup_reply();
  ob_flush();
  return 0;
}

static void reset_state(void) {
  for (int i = 0; i < MAX_DRAWABLES; i++) if (g_drw[i].used) { free(g_drw[i].fb); memset(&g_drw[i], 0, sizeof g_drw[i]); }
  for (int i = 0; i < MAX_GCS; i++) memset(&g_gc[i], 0, sizeof g_gc[i]);
  for (int i = 0; i < MAX_PROPS; i++) if (g_prop[i].used) { free(g_prop[i].data); memset(&g_prop[i], 0, sizeof g_prop[i]); }
  g_ncmaps = 0; g_drw_seq = 0; g_seq = 0; g_bigreq = 0; g_dirty = 0;
  g_present_win = 0; g_ib_len = 0; g_ob_len = 0; g_glx_ctx_tag = 0;
  g_glx_drawable = 0; g_glxwin_id = 0; g_glxwin_x = 0;
  g_glx_last_present_ms = 0; g_glx_swaps = 0;

  // The root window exists implicitly.
  Drw* root = d_new(ROOT_WIN);
  if (root) {
    root->is_window = 1; root->depth = 24; root->class_ = 1;
    root->w = SCREEN_W; root->h = SCREEN_H; root->mapped = 1;
    root->parent = 0; root->visual = VISUAL_24; root->colormap = ROOT_CMAP;
    root->bg_pixel = 0x000000;
    root->fb = NULL;                                    // never presented directly
  }
}


// ---------------------------------------------------------------------------
// input: host events -> X11 KeyPress / ButtonPress / MotionNotify
// ---------------------------------------------------------------------------
// The X server had no input at all: xeyes only ever worked because it POLLS QueryPointer. Events
// come from the same evdev-normalised queue the Wayland compositor drains (gpu.h), so the
// translation done once in window.m serves both.
//
// X keycodes are evdev + 8 — the historical offset baked into every XKB keymap.
#define EV_KEY_PRESS       0x00000001u
#define EV_KEY_RELEASE     0x00000002u
#define EV_BUTTON_PRESS    0x00000004u
#define EV_BUTTON_RELEASE  0x00000008u
#define EV_POINTER_MOTION  0x00000040u
#define EV_BUTTON_MOTION   0x00002000u

#define X_KEY_PRESS      2
#define X_KEY_RELEASE    3
#define X_BUTTON_PRESS   4
#define X_BUTTON_RELEASE 5
#define X_MOTION_NOTIFY  6

static uint16_t g_kbd_state;                 // X modifier mask, tracked across events

// The window input goes to: the deepest mapped InputOutput window that asked for this event.
// One client with one or two windows is the whole world here, so "last created that wants it"
// is a good enough approximation of the real pointer-window walk.
static Drw* input_target(uint32_t mask) {
  Drw* best = NULL;
  for (int i = 0; i < MAX_DRAWABLES; i++) {
    Drw* d = &g_drw[i];
    if (!d->used || !d->is_window || !d->mapped) continue;
    if (d->class_ == 2) continue;                          // InputOnly
    if (!(d->event_mask & mask)) continue;
    if (!best || d->seq > best->seq) best = d;
  }
  if (!best) {
    static int once;
    if (!once++) {
      xwarn("input: no window selected mask %#x; window masks are:", mask);
      for (int i = 0; i < MAX_DRAWABLES; i++)
        if (g_drw[i].used && g_drw[i].is_window)
          xwarn("  win 0x%x mapped=%d class=%u evmask=%#x",
                g_drw[i].id, g_drw[i].mapped, g_drw[i].class_, g_drw[i].event_mask);
    }
  }
  return best;
}

static void send_input_event(uint8_t type, uint8_t detail, Drw* win, int x, int y) {
  uint8_t e[32]; memset(e, 0, sizeof e);
  e[0] = type;
  e[1] = detail;
  p16(e + 2, (uint16_t)g_seq);                             // sequence number
  { struct timeval tv; gettimeofday(&tv, NULL);
    p32(e + 4, (uint32_t)(tv.tv_sec * 1000u + tv.tv_usec / 1000u)); }   // time
  p32(e + 8, ROOT_WIN);                                 // root
  p32(e + 12, win->id);                                    // event window
  p32(e + 16, 0);                                          // child = None
  p16(e + 20, (uint16_t)(int16_t)x);                       // root_x
  p16(e + 22, (uint16_t)(int16_t)y);                       // root_y
  p16(e + 24, (uint16_t)(int16_t)(x - win->x));            // event_x
  p16(e + 26, (uint16_t)(int16_t)(y - win->y));            // event_y
  p16(e + 28, g_kbd_state);                                // modifier/button state
  e[30] = 1;                                               // same_screen
  send_event(e);
  xlog("input -> win 0x%x type=%u detail=%u at %d,%d state=%#x",
       win->id, type, detail, x, y, g_kbd_state);
}

// X modifier bits: Shift 1, Lock 2, Control 4, Mod1 8; buttons 1<<8 upward.
static void x11_pump_input(void) {
  struct gpuwin_event g;
  while (gpuwin_next_event(&g)) {
    uint16_t mods = 0;
    if (g.mods & GPUWIN_MOD_SHIFT) mods |= 1;
    if (g.mods & GPUWIN_MOD_CAPS)  mods |= 2;
    if (g.mods & GPUWIN_MOD_CTRL)  mods |= 4;
    if (g.mods & GPUWIN_MOD_ALT)   mods |= 8;
    g_kbd_state = (uint16_t)((g_kbd_state & 0xFF00) | mods);

    switch (g.type) {
    case GPUWIN_EV_KEY: {
      Drw* w = input_target(g.state ? EV_KEY_PRESS : EV_KEY_RELEASE);
      if (!w) break;
      send_input_event(g.state ? X_KEY_PRESS : X_KEY_RELEASE,
                       (uint8_t)(g.code + 8),              // evdev -> X keycode
                       w, g.x, g.y);
      break;
    }
    case GPUWIN_EV_BUTTON: {
      uint8_t btn = g.code == 0x111 ? 3 : (g.code == 0x112 ? 2 : 1);   // right/middle/left
      Drw* w = input_target(g.state ? EV_BUTTON_PRESS : EV_BUTTON_RELEASE);
      if (w) send_input_event(g.state ? X_BUTTON_PRESS : X_BUTTON_RELEASE, btn, w, g.x, g.y);
      if (g.state) g_kbd_state |= (uint16_t)(1u << (7 + btn));
      else         g_kbd_state &= (uint16_t)~(1u << (7 + btn));
      break;
    }
    case GPUWIN_EV_SCROLL: {
      // X has no scroll axis: wheel up/down are buttons 4 and 5, as a press/release pair.
      if (!g.y) break;
      uint8_t btn = g.y < 0 ? 4 : 5;
      Drw* w = input_target(EV_BUTTON_PRESS);
      if (!w) break;
      int n = g.y < 0 ? -g.y : g.y;
      for (int i = 0; i < n && i < 8; i++) {
        send_input_event(X_BUTTON_PRESS,   btn, w, g.x, g.y);
        send_input_event(X_BUTTON_RELEASE, btn, w, g.x, g.y);
      }
      break;
    }
    case GPUWIN_EV_MOTION: {
      Drw* w = input_target(EV_POINTER_MOTION | EV_BUTTON_MOTION);
      if (w) send_input_event(X_MOTION_NOTIFY, 0, w, g.x, g.y);
      break;
    }
    default: break;
    }
  }
}



static void serve_client(void) {
  reset_state();
  atoms_init();
  if (do_setup() < 0) return;

  while (g_running) {
    struct pollfd pf = { g_cfd, POLLIN, 0 };
    int timeout = g_dirty ? 8 : 16;          // 16 ms idle: bounds input latency
    int nr = poll(&pf, 1, timeout);
    if (nr < 0) { if (errno == EINTR) continue; break; }
    x11_pump_input();
    // Input events are generated OUTSIDE request handling, and ob_flush() below only runs after
    // a request arrives — so without this an idle client never receives them: they sit in the
    // output buffer until it happens to send something of its own.
    ob_flush();
    if (nr == 0) {
      if (g_dirty) { do_present(); g_dirty = 0; }
      continue;
    }
    if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      if (!(pf.revents & POLLIN)) break;
    }
    int r = ib_fill();
    if (r == 0 || r < 0) break;

    // process every complete request in the buffer
    size_t off = 0;
    for (;;) {
      if (g_ib_len - off < 4) break;
      const uint8_t* h = g_ib + off;
      uint8_t op = h[0], detail = h[1];
      uint32_t units = g16(h + 2);
      size_t hdr = 4, total;
      if (units == 0) {
        if (!g_bigreq) { xwarn("zero-length request opcode %u without BIG-REQUESTS", op); off = g_ib_len; break; }
        if (g_ib_len - off < 8) break;
        uint32_t big = g32(h + 4);
        if (big < 2) { off = g_ib_len; break; }
        total = (size_t)big * 4;
        hdr = 8;
      } else {
        total = (size_t)units * 4;
      }
      if (g_ib_len - off < total) break;                // wait for the rest
      g_seq++;
      handle_request(op, detail, h + hdr, total - hdr);
      off += total;
    }
    ib_consume(off);
    ob_flush();

    // coalesce: present only once the client goes momentarily quiet
    if (g_dirty) {
      struct pollfd q = { g_cfd, POLLIN, 0 };
      if (poll(&q, 1, 8) == 0) { do_present(); g_dirty = 0; }
    }
  }
  ob_flush();
  xlog("client disconnected");
}

static void* srv_thread(void* arg) {
  (void)arg;
  while (g_running) {
    struct pollfd pf = { g_lfd, POLLIN, 0 };
    int nr = poll(&pf, 1, 200);
    if (nr <= 0) { if (nr < 0 && errno != EINTR) break; continue; }
    int fd = accept(g_lfd, NULL, NULL);
    if (fd < 0) { if (errno == EINTR) continue; break; }
    xlog("client connected (fd %d)", fd);
    g_cfd = fd;
    serve_client();
    close(g_cfd);
    g_cfd = -1;
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// public entry points
// ---------------------------------------------------------------------------
int x11srv_start(const char* socket_path) {
  if (!socket_path || !*socket_path) return -1;
  if (g_running) return 0;

  snprintf(g_sockpath, sizeof g_sockpath, "%s", socket_path);
  unlink(g_sockpath);

  // ensure the parent directory exists (e.g. /tmp/rdx/.X11-unix)
  char dir[256];
  snprintf(dir, sizeof dir, "%s", g_sockpath);
  char* slash = strrchr(dir, '/');
  if (slash && slash != dir) { *slash = 0; mkdir(dir, 0777); }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) { xwarn("socket: %s", strerror(errno)); return -1; }
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof sa.sun_path, "%s", g_sockpath);
  if (bind(fd, (struct sockaddr*)&sa, sizeof sa) < 0) {
    xwarn("bind %s: %s", g_sockpath, strerror(errno));
    close(fd); return -1;
  }
  if (listen(fd, 4) < 0) { xwarn("listen: %s", strerror(errno)); close(fd); return -1; }
  chmod(g_sockpath, 0777);

  g_lfd = fd;
  g_running = 1;
  if (pthread_create(&g_thread, NULL, srv_thread, NULL) != 0) {
    xwarn("pthread_create failed");
    g_running = 0; close(fd); g_lfd = -1; return -1;
  }
  fprintf(stderr, "[x11] server listening on %s\n", g_sockpath);
  return 0;
}

void x11srv_stop(void) {
  if (!g_running) return;
  g_running = 0;
  if (g_cfd >= 0) { shutdown(g_cfd, SHUT_RDWR); }
  if (g_lfd >= 0) { close(g_lfd); g_lfd = -1; }
  pthread_join(g_thread, NULL);
  unlink(g_sockpath);
}

// The X11 twin of wlsrv_restart_after_fork(); see that comment for why this exists at all.
// x11srv_stop() is doubly wrong in a fork child: it pthread_join()s a thread that does not exist
// there (fork keeps only the caller), and it unlink()s g_sockpath, which names the PARENT's
// socket. So tear down by hand, without joining and without unlinking, and rebind on a path the
// caller has keyed to the child's own pid.
int x11srv_restart_after_fork(const char* socket_path) {
  if (g_cfd >= 0) { close(g_cfd); g_cfd = -1; }
  if (g_lfd >= 0) { close(g_lfd); g_lfd = -1; }
  g_running = 0;                       // ... so x11srv_start()'s "already running" guard lets go
  return x11srv_start(socket_path);
}
