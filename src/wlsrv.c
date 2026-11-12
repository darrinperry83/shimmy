// wlsrv.c — in-process Wayland compositor.
//
// WIRE FORMAT
//   Every message is  { u32 object_id; u32 (size << 16) | opcode; args... }  where `size` counts
//   the whole message including the 8-byte header and is a multiple of 4. Argument encodings:
//     int/uint/object/new_id  one u32
//     fixed                   one u32, signed 24.8
//     string                  u32 length INCLUDING the NUL, then that many bytes, padded to 4
//     array                   u32 length, then that many bytes, padded to 4
//     fd                      NOT in the byte stream at all — passed out of band via SCM_RIGHTS
//
// The fd rule is what makes wl_shm work: the client creates an anonymous file (memfd_create,
// which syscalls_ext.c implements as mkstemp+unlink), mmaps it, and sends the descriptor over
// the socket. Because the guest lives in OUR address space and net.c rebuilds Linux cmsghdrs
// into Darwin ones, that descriptor arrives here as an ordinary host fd we can mmap directly:
// the client's pixels and ours are literally the same pages, so a "buffer" costs no copy.
//
// SCOPE
//   One client, one toplevel surface, presented to the single Metal window. Enough for a real
//   EGL/GLES client; multi-surface compositing (subsurfaces, popups) is not implemented.
#include "wlsrv.h"
#include "x11srv.h"                       // x11rnd_open/present — the generic Metal forwarders
#include "gpu.h"                          // gpuwin_next_event — host input
#include "capresent.h"                    // WLSRV_CALAYER — the IOSurface/CALayer alternative
#include "multiwindow_gate.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <sys/un.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <Accelerate/Accelerate.h>       // vImage — macOS's own SIMD+threaded pixel engine
#include "rd_output2_policy.h"
#include "present_feedback.h"
#include "drmasahi.h"                     // drm_ioctl — PRIME_FD_TO_HANDLE for dma-buf import
#include "rdgpu_surface_handoff.h"        // rdgpu_handoff_import_init — cross-process mach-port registry

#ifndef WL_HEADLESS
extern int rd_secondary_open(uint32_t, uint32_t, uint32_t, const char*, uint32_t);
extern int rd_secondary_present(uint32_t, const uint8_t*, uint32_t, uint32_t);
extern int rd_secondary_direct_consumed(uint32_t);
extern int rd_secondary_direct_claim(uint32_t);
extern int rd_secondary_popup_present(uint32_t, uint32_t, int32_t, int32_t,
                                      const uint8_t*, uint32_t, uint32_t);
extern void rd_secondary_popup_close(uint32_t);
extern void rd_secondary_close(uint32_t);
extern int rd_secondary_next_event(uint32_t*, struct gpuwin_event*);
extern uint32_t rd_secondary_key_window_id(void);
extern int rd_guest_interactive(uint32_t, uint32_t, int32_t, int32_t);
#endif

#ifndef WL_HEADLESS
static int g_multiwindow_failed;
#endif
static int multiwindow_enabled(void) {
#ifdef WL_HEADLESS
  return 0;
#else
  static atomic_int enabled = -1;
  static atomic_int ready = 0;
  static atomic_ullong nextCheckNS = 0;
  int configured = atomic_load(&enabled);
  if (configured < 0) {
    const char* value = getenv("RDWL_MULTIWINDOW");
    configured = value && !strcmp(value, "1");
    int unset = -1;
    if (atomic_compare_exchange_strong(&enabled, &unset, configured) && configured)
      fprintf(stderr, "[wl] EXPERIMENTAL detached secondary windows configured\n");
  }
  if (!configured || g_multiwindow_failed) return 0;
  const char* marker = getenv("RDWL_MULTIWINDOW_READY_PATH");
  if (!marker || !*marker) return 1; // original opt-in behavior
  if (atomic_load(&ready)) return 1;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
  unsigned long long next = atomic_load(&nextCheckNS);
  if (ns < next || !atomic_compare_exchange_strong(&nextCheckNS, &next,
                                                   ns + 30000000ull)) return 0;
  // Exactly one caller checks the marker in each 30 ms interval.
  if (!rd_multiwindow_gate_file_ready(marker)) return 0;
  char nonce[17];
  if (!rd_multiwindow_gate_read_nonce(marker, nonce)) return 0;
  if (!atomic_exchange(&ready, 1)) {
    fprintf(stderr, "[wl] detached secondary windows enabled by private launcher marker\n");
    // Readiness is a one-byte event, not a guessed sleep. The launcher passes
    // only this write end to its owned shim and waits on the read end before
    // issuing the one-shot output create command.
    const char* ack = getenv("RDWL_MULTIWINDOW_ACK_FD");
    if (ack && *ack) {
      char* end = NULL;
      long fd = strtol(ack, &end, 10);
      if (end && !*end && fd >= 3 && fd <= 1048576) {
        ssize_t n;
        do { n = write((int)fd, nonce, sizeof nonce); } while (n < 0 && errno == EINTR);
        if (n != (ssize_t)sizeof nonce)
          fprintf(stderr,"[wl] multiwindow readiness pipe write failed\n");
        close((int)fd);
      }
    }
  }
  return 1;
#endif
}

// ---------------------------------------------------------------------------
// WLSRV_PROF=1 — per-phase timing of the pixel path.
//
// This exists because "the compositor costs 25 ms/frame" was a SUM, and a sum cannot tell you
// which of convert / composite / present / dump is paying it. Every phase is timed separately and
// reported as min/p50/p95 over the whole run, because min is the only figure contention cannot
// inflate. Off by default and costing one atomic load per frame when off.
// ---------------------------------------------------------------------------
#define PROF_CAP 4096
enum { PH_CONVERT, PH_COPY, PH_BLEND, PH_PRESENT, PH_DUMP, PH_FRAME, PH_PERIOD, PH_N };
static const char* g_ph_name[PH_N] =
  { "convert", "copy", "blend", "present", "dump", "recomposite", "frame-period" };
static int      g_prof = -1;
static float    g_ph[PH_N][PROF_CAP];
static int      g_ph_n[PH_N];
static double   g_ph_sum[PH_N];
static uint64_t g_ph_cnt[PH_N];
static double   g_ns_per_tick = 0;

// WLSRV_SCALAR=1 forces the pre-vImage hand-written loops back on. It exists so the speedup can
// be A/B'd with ONE BINARY and one variable moved — "a speedup you cannot show going away is not
// measured" — and it keeps the fallback path compiled and runnable rather than dead.
static int scalar_forced(void) {
  static int v = -1;
  if (v < 0) { const char* e = getenv("WLSRV_SCALAR"); v = (e && *e && *e != '0') ? 1 : 0; }
  return v;
}

// Should vImage split this copy across cores? IT DEPENDS ON THE SURFACE SIZE, and getting that
// wrong is a real cost on ordinary windows.
//
// History, because the default here has already been wrong once. Forwarding the pixel loops to
// vImage made the TAIL much better and the TYPICAL frame slightly worse; the cause is vImage's own
// multithreading — the compositor thread borrows several cores for ~0.1 ms, and on a frame that
// was never going to stall, the cores taken from the CLIENT cost more than the copy saves. So
// `WLSRV_NOTILE=1` was added to test that rather than argue it, and it confirmed the mechanism at
// 2048x2048.
//
// P3 then caught the part I had missed: the trade REVERSES with size, so a single always-tile
// default is wrong for a typical window. Measured, zink, min-of-3, on the one statistic both our
// instruments agree on (the mean — our percentile definitions differ because we sample at
// different points in the frame, so those are NOT comparable across harnesses):
//
//     1.0 Mpx (1024^2)   tiled 0.781   notile 0.701 ms    notile 10% BETTER
//     1.9 Mpx (1408^2)   tiled 0.884   notile 0.847 ms    notile  4% better
//     2.8 Mpx (1728^2)   tiled 1.020   notile 1.017 ms    a tie
//     4.0 Mpx (2048^2)   tiled 1.328   notile 1.468 ms    tiled  10% BETTER
//
// Independently corroborated at 1.0 Mpx by P3's in-ICD timer (notile 7.6% better) and at 4.0 Mpx
// by my own earlier round through that same timer (tiled ~9% better). The crossover is ~2.8 Mpx:
// below it vImage's threading is pure overhead, above it the parallel copy pays for itself.
//
// So the default is now SIZE-AWARE rather than always-on. Overrides exist for measurement and to
// keep both arms runnable: WLSRV_NOTILE=1 forces tiling off, WLSRV_TILE=1 forces it on.
static int prof_on(void);                     // defined below; used only by the one-shot log
// The crossover, in pixels. It is a MEASURED CONSTANT FROM ONE MACHINE AND ONE DRIVER, not a law,
// so WLSRV_TILE_MIN_MPX overrides it without a rebuild.
//
// IT IS ALSO KNOWN TO BE WRONG FOR THE SOFTWARE PATH, and that is worth stating where the number
// lives rather than in a commit message. Measured with each arm forced and proven by the log line
// below, min-of-5:
//
//                       1.0 Mpx              4.0 Mpx
//     zink/MoltenVK     notile 10% better    TILED 10% better
//     llvmpipe          notile  4% better    notile 7.4% better
//
// So above the threshold the two drivers want opposite things, and the compositor cannot tell them
// apart -- it sees an shm buffer either way. The default favours zink because that is the GPU path
// most runs use; a software-only deployment should just set WLSRV_NOTILE=1, which is better for
// llvmpipe at every size either of us has measured. (P3 found this; my first llvmpipe A/B reported
// "no effect" because, as they spotted from the numbers alone, my notile arm equalled my own tiled
// arm. Hence the log line below: an A/B whose arms cannot be told apart in the log is not an A/B.)
// 2800*1024 px = 2.73 Mpx. The sweep put the tie at 1728^2 = 2.85 Mpx, so the threshold sits
// just below the measured crossover -- deliberately, since below it notile wins on BOTH drivers
// and above it only zink prefers tiling.
#define VIMG_TILE_MIN_PX_DEFAULT (2800u * 1024u)
static size_t vimg_tile_min_px(void) {
  static size_t v;
  if (!v) {
    const char* e = getenv("WLSRV_TILE_MIN_MPX");
    double m = e ? atof(e) : 0.0;
    v = (m > 0) ? (size_t)(m * 1048576.0) : (size_t)VIMG_TILE_MIN_PX_DEFAULT;
  }
  return v;
}
static vImage_Flags vimg_flags_px(size_t px) {
  static int force = -2;                       // -1 = never tile, 1 = always tile, 0 = by size
  if (force == -2) {
    const char* no = getenv("WLSRV_NOTILE");
    const char* yes = getenv("WLSRV_TILE");
    force = (no && *no && *no != '0') ? -1 : ((yes && *yes && *yes != '0') ? 1 : 0);
  }
  vImage_Flags f = (force < 0) ? kvImageDoNotTile
                 : (force > 0) ? kvImageNoFlags
                 : ((px >= vimg_tile_min_px()) ? kvImageNoFlags : kvImageDoNotTile);
  // SAY WHICH ARM ACTUALLY RAN. An A/B where both arms silently do the same thing produces two
  // matching numbers and reads as "no effect" -- which is exactly what my first llvmpipe result
  // looked like, and P3 spotted it from outside by noticing my notile number equalled my own
  // tiled number. A knob that cannot be seen in the log is not a knob you can measure with, so
  // this states the decision once per process rather than leaving it to be inferred.
  static int said;
  if (!said && prof_on()) {
    said = 1;
    fprintf(stderr, "[wl] vImage tiling: %s (%.2f Mpx first buffer, threshold %.2f Mpx) -> %s\n",
            force < 0 ? "FORCED OFF (WLSRV_NOTILE)"
                      : force > 0 ? "FORCED ON (WLSRV_TILE)" : "auto by size",
            (double)px / 1048576.0, (double)vimg_tile_min_px() / 1048576.0,
            f == kvImageNoFlags ? "TILED" : "not tiled");
  }
  return f;
}

static int prof_on(void) {
  if (g_prof < 0) {
    const char* e = getenv("WLSRV_PROF");
    g_prof = (e && *e && *e != '0') ? 1 : 0;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    g_ns_per_tick = (double)tb.numer / (double)tb.denom;
  }
  return g_prof;
}
static inline uint64_t prof_t0(void) { return prof_on() ? mach_absolute_time() : 0; }
static void prof_add(int ph, uint64_t t0) {
  if (!t0) return;
  double ms = (double)(mach_absolute_time() - t0) * g_ns_per_tick / 1e6;
  g_ph_sum[ph] += ms; g_ph_cnt[ph]++;
  if (g_ph_n[ph] < PROF_CAP) g_ph[ph][g_ph_n[ph]++] = (float)ms;
}
static int prof_cmp(const void* a, const void* b) {
  float x = *(const float*)a, y = *(const float*)b;
  return (x < y) ? -1 : (x > y);
}
static void prof_report(void) {
  if (!prof_on()) return;
  fprintf(stderr, "[prof] phase        n      min       p50       p95      mean   total_ms\n");
  for (int p = 0; p < PH_N; p++) {
    int n = g_ph_n[p];
    // A PHASE THAT DID NOT RUN IS THE RESULT, so it gets a row saying so rather than vanishing.
    // These rows used to be skipped, which makes "the CA path deleted the copy" and "the profiler
    // is broken" produce the identical table — and S2, who reads this log and not this file, has
    // no way to tell those apart.
    if (!n) {
      fprintf(stderr, "[prof] %-11s %5d %8s %9s %9s %9s %10s   <- phase did not execute\n",
              g_ph_name[p], 0, "-", "-", "-", "-", "-");
      continue;
    }
    static float tmp[PROF_CAP];
    memcpy(tmp, g_ph[p], sizeof(float) * (size_t)n);
    qsort(tmp, (size_t)n, sizeof(float), prof_cmp);
    fprintf(stderr, "[prof] %-11s %5llu %8.3f %9.3f %9.3f %9.3f %10.1f\n",
            g_ph_name[p], (unsigned long long)g_ph_cnt[p], tmp[0], tmp[n / 2],
            tmp[(int)(n * 0.95)], g_ph_sum[p] / (double)g_ph_cnt[p], g_ph_sum[p]);
  }
  // WHICH ARM PRODUCED THE TABLE ABOVE. Two presentation paths writing identically-formatted
  // [prof] tables is how one arm's numbers get filed under the other's name — the failure this
  // project has already had with WLSRV_NOTILE. The presentation path names itself, every report.
  fprintf(stderr, "[prof] presentation path: %s\n",
          capres_enabled() ? "CALayer/IOSurface (WLSRV_CALAYER)"
                           : "Metal staging texture + CAMetalLayer drawable");
  capres_report();
  fflush(stderr);
}

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
static int g_verbose = -1;
static void wlog(const char* fmt, ...) {
  if (g_verbose < 0) { const char* e = getenv("WLSRV_DEBUG"); g_verbose = (e && *e && *e != '0'); }
  if (!g_verbose) return;
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[wl] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
  va_end(ap);
}
static void wwarn(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[wl] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
  va_end(ap);
}

// ---------------------------------------------------------------------------
// object table
// ---------------------------------------------------------------------------
enum iface {
  IF_NONE = 0, IF_DISPLAY, IF_REGISTRY, IF_CALLBACK, IF_COMPOSITOR, IF_SHM, IF_SHM_POOL,
  IF_BUFFER, IF_SURFACE, IF_REGION, IF_SEAT, IF_POINTER, IF_KEYBOARD, IF_TOUCH, IF_OUTPUT,
  IF_WM_BASE, IF_XDG_SURFACE, IF_XDG_TOPLEVEL, IF_XDG_POSITIONER, IF_XDG_POPUP,
  IF_SUBCOMPOSITOR, IF_SUBSURFACE, IF_DDM,
  IF_DMABUF, IF_DMABUF_PARAMS, IF_DMABUF_FEEDBACK,
  IF_SERVER_DECORATION_MANAGER, IF_SERVER_DECORATION,
  IF_DATA_SOURCE, IF_DATA_DEVICE, IF_DATA_OFFER,
  // Protocols KWin's nested Wayland backend REQUIRES just to start (WaylandDisplay::initialize()
  // returns false when any of these globals is absent, before it ever creates an output):
  IF_SINGLE_PIXEL_MANAGER, IF_SINGLE_PIXEL_BUFFER,
  IF_VIEWPORTER, IF_VIEWPORT,
  IF_POINTER_CONSTRAINTS, IF_LOCKED_POINTER, IF_CONFINED_POINTER,
  IF_PRESENTATION, IF_PRESENTATION_FEEDBACK
};

#define MAX_OBJ      8192                 // libwayland hands out client ids sequentially from 2
#define SERVER_ID_LO 0xff000000u

struct object {
  uint8_t  iface;
  uint32_t version;
  uint32_t data;                          // index into the pool/buffer/surface arrays, or 0
};
static struct object g_obj[MAX_OBJ];
static struct object g_server_obj[64];

static struct object* obj_get(uint32_t id) {
  struct object* o = id >= SERVER_ID_LO && id - SERVER_ID_LO < 64
    ? &g_server_obj[id - SERVER_ID_LO] : (id > 0 && id < MAX_OBJ ? &g_obj[id] : NULL);
  return o && o->iface ? o : NULL;
}
static void obj_set(uint32_t id, enum iface i, uint32_t ver, uint32_t data) {
  struct object* o = id >= SERVER_ID_LO && id - SERVER_ID_LO < 64
    ? &g_server_obj[id - SERVER_ID_LO] : (id > 0 && id < MAX_OBJ ? &g_obj[id] : NULL);
  if (!o) { wwarn("object id %u out of range", id); return; }
  o->iface = (uint8_t)i; o->version = ver; o->data = data;
}
static void obj_del(uint32_t id) {
  if (id >= SERVER_ID_LO && id - SERVER_ID_LO < 64) memset(&g_server_obj[id - SERVER_ID_LO], 0, sizeof g_server_obj[0]);
  else if (id < MAX_OBJ) memset(&g_obj[id], 0, sizeof g_obj[0]);
}

// ---------------------------------------------------------------------------
// resources
// ---------------------------------------------------------------------------
// A pool OUTLIVES its wl_shm_pool object. The protocol is explicit that
// "the mmapped memory will be released when all buffers that have been created from this pool
// are gone", and Mesa relies on it: it creates a pool, creates one buffer, and destroys the pool
// immediately, then keeps drawing into that buffer for the life of the surface. Unmapping on
// destroy (and recycling the slot) leaves every buffer pointing at freed memory.
#define MAX_POOL 32
struct shmpool {
  int used, fd;
  int refs;                               // buffers still alive from this pool
  int destroyed;                          // the wl_shm_pool object is gone
  uint8_t* map; size_t size;
};
static struct shmpool g_pool[MAX_POOL];

static void pool_maybe_free(int slot) {
  if (slot < 0 || slot >= MAX_POOL) return;
  struct shmpool* p = &g_pool[slot];
  if (!p->used || !p->destroyed || p->refs > 0) return;
  if (p->map) munmap(p->map, p->size);
  if (p->fd >= 0) close(p->fd);
  memset(p, 0, sizeof *p);
}

#define MAX_BUF 64
struct wbuffer {
  int used, pool;
  int32_t offset, width, height, stride;
  uint32_t format, id;
  int dmabuf;                 // 1 = zwp_linux_dmabuf_v1 buffer (no shm pool behind it)
  int dmabuf_fd;              // imported dma-buf fd, owned until the buffer is destroyed
  uint32_t dmabuf_handle;     // opaque GEM handle returned by PRIME_FD_TO_HANDLE
};
static struct wbuffer g_buf[MAX_BUF];

// ---------------------------------------------------------------------------
// zwp_linux_dmabuf_v1 / zwp_linux_buffer_params_v1
//
// Standard dma-buf import (DMA_BUF_TARGET.md). The client sends one plane fd with `add`
// (out of band via SCM_RIGHTS), then create/create_immed. The fd is resolved to an opaque
// GEM handle through the shim's DRM render node (drmasahi.c); the real IOSurface backing is
// wired in a separate task, so for now a successful import only records the handle.
// ---------------------------------------------------------------------------
#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_IOCTL_PRIME_FD_TO_HANDLE 0xC00C642Eu
#define DRM_IOCTL_GEM_CLOSE          0x40086409u
struct drm_prime_handle { uint32_t handle; uint32_t flags; int32_t fd; };
struct drm_gem_close    { uint32_t handle; uint32_t pad; };

// The shim has no real /dev/dri/renderD128 on macOS; drm_ioctl never inspects the fd's
// contents, so a synthetic descriptor (a /dev/null handle) stands in for the render node.
static int dmabuf_render_fd(void) {
  static int fd = -1;
  if (fd < 0) fd = open("/dev/null", O_RDWR);
  return fd;
}

// zwp_linux_dmabuf_v1 v4 get_default_feedback support (aquamarine 0.16+ REQUIRES it).
//
// Stock aquamarine's Wayland backend does not use the v1..v3 `format`/`modifier` events for its
// allocator: it calls get_default_feedback() and reads `main_device` (a dev_t) + `format_table`
// (an fd to a packed {drm_format, pad, modifier}[] array). From main_device it resolves the DRM
// render node via drmGetDeviceFromDevId() and opens it for a GBM allocator. Without the feedback
// object, drmState.nodeName stays empty, drmFD() returns -1, and CBackend::start() dies with
// "Cannot open backend: no allocator available" — the exact CBackend::create() failure this fixes.
//
// main_device is the dev_t the shim's drm_fake_stat() reports for /dev/dri/renderD128:
// (DRM_MAJOR 226 << 8) | 128 == 0xE280, in the 8-byte aarch64 dev_t layout.
static int dmabuf_format_table_fd(void) {
  // { uint32_t drmFormat; char pad[4]; uint64_t modifier; } — 16 bytes per entry, both LINEAR.
  static const uint32_t fmt[2] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888 };
  uint8_t table[32];
  memset(table, 0, sizeof table);               // pad + modifier(0 == DRM_FORMAT_MOD_LINEAR) are zero
  for (int i = 0; i < 2; i++) { table[i * 16] = (uint8_t)fmt[i]; table[i * 16 + 1] = (uint8_t)(fmt[i] >> 8);
                                 table[i * 16 + 2] = (uint8_t)(fmt[i] >> 16); table[i * 16 + 3] = (uint8_t)(fmt[i] >> 24); }
  // shm_open() is not reliable here (fails under the shim's environment); use an unlinked temp
  // file instead — the guest only read()s this fd, so a plain file is fine.
  const char* td = getenv("TMPDIR");
  char tmpl[PATH_MAX];
  if (td && *td) snprintf(tmpl, sizeof tmpl, "%s%srddmabuf-XXXXXX", td,
                          td[strlen(td) - 1] == '/' ? "" : "/");
  else           snprintf(tmpl, sizeof tmpl, "/var/tmp/rddmabuf-XXXXXX");
  int fd = mkstemp(tmpl);
  if (fd < 0) return -1;
  unlink(tmpl);
  if (write(fd, table, sizeof table) != (ssize_t)sizeof table) { close(fd); return -1; }
  lseek(fd, 0, SEEK_SET);
  return fd;
}

#define MAX_DMABUF_PARAMS 16
struct dmabuf_params {
  int used;
  int fd;                     // -1 until add()
  int have;                   // one plane has been added
  uint32_t stride, offset;
};
static struct dmabuf_params g_dmabuf_params[MAX_DMABUF_PARAMS];

#define MAX_FRAME 32
struct surface {
  int used;
  uint32_t id;
  uint32_t pending_buffer, current_buffer;    // wl_buffer object ids (0 = none)
  int      has_pending_buffer;                // attach(NULL) is different from "no attach"
  uint32_t frame_cb[MAX_FRAME]; int nframe;
  uint32_t direct_frame_due_ms;                // opt-in paced direct-GPU callback
  uint64_t direct_frame_target;                // Metal-presented sequence, opt-in
  unsigned direct_frame_output;                // 0 primary, 1 WAYLAND-2
  uint32_t xdg_surface, xdg_toplevel, xdg_popup;
  uint32_t parent_surface;                   // xdg_toplevel.set_parent, for detached host ordering
  uint32_t popup_owner;                      // owning toplevel for detached popup presentation
  char     title[256];
  int      is_popup;                          // has the xdg_popup role, not the toplevel one
  int      acked;
  // --- popup state (xdg_popup only) -----------------------------------------------------------
  // A popup is a SEPARATE surface living on top of the toplevel at an offset the client computes
  // with an xdg_positioner. It is not a window: it must never take the host window's size, and it
  // must never permanently own input focus — but it must be DRAWN, and it must be hit-tested,
  // or a menu opens invisibly and swallows nothing.
  int32_t  rx, ry;                            // position relative to the parent (what configure carries)
  int32_t  px, py;                            // top-left in TOPLEVEL-local coordinates (compositing)
  int32_t  pw, ph;                            // size the positioner asked for
  int      mapped;                            // has committed a buffer we have composited
  uint32_t stack;                             // creation order; higher is on top
  // --- subsurface state (wl_subsurface only) ------------------------------------------------
  // KWin's nested Wayland output draws its desktop as a single-pixel background on the main
  // toplevel plus a flat layer of wl_subsurfaces for the actual content. Each subsurface is
  // positioned at (parent position + set_position); `subs_z` orders the layer (higher is on top).
  int      is_subsurface;                     // has the wl_subsurface role
  uint32_t subsurface_parent;                 // parent wl_surface id (0 = none)
  int32_t  subs_x, subs_y;                    // wl_subsurface.set_position, parent-local
  uint32_t subs_z;                            // z-order among a parent's subsurfaces
  // wp_viewport.set_destination gives a buffer-less surface (single-pixel) its logical size.
  int32_t  vp_w, vp_h;                        // 0 = unset
  // Every surface keeps its OWN converted pixels. A single global scratch cannot work once two
  // surfaces are on screen at once: the menu's frame would erase the window under it, and the
  // window's next frame would erase the menu.
  uint8_t* rgba; size_t rgba_cap;
  int32_t  rgba_w, rgba_h;
};
#define MAX_SURFACE 16
static struct surface g_surf[MAX_SURFACE];
static uint32_t g_stack_next = 1;
static uint32_t g_substack_next = 1;

// An xdg_positioner is the client's request for WHERE the popup goes. Parsing only set_size gives
// a popup a size and no position, which stacks every menu in the top-left corner.
#define MAX_POS 32
struct positioner {
  int used;
  int32_t w, h;                               // set_size
  int32_t ax, ay, aw, ah;                     // set_anchor_rect
  uint32_t anchor, gravity;                   // set_anchor / set_gravity
  int32_t ox, oy;                             // set_offset
};
static struct positioner g_pos[MAX_POS];

// One clipboard selection can be active, but toolkits create several source
// objects over a session. Offers are server-created Wayland objects.
#define MAX_DATA_SOURCE 16
struct data_source { uint32_t id; char mime[16][128]; int nmime; };
static struct data_source g_sources[MAX_DATA_SOURCE];
static uint32_t g_data_devices[16];
static int g_ndevices;
static uint32_t g_selection_source;
static uint32_t g_offer_next = SERVER_ID_LO;
static atomic_int g_guest_pasteboard_count;
static void import_selection_from_mac(void);

static struct surface* surface_of(uint32_t id) {
  struct object* o = obj_get(id);
  if (!o || o->iface != IF_SURFACE || o->data >= MAX_SURFACE) return NULL;
  return &g_surf[o->data];
}

// The one real window. Everything else — cursor surfaces, popups, role-less scratch — is not.
// The PRIMARY toplevel: the first one created. It owns the host window's size, because we have
// exactly one NSWindow and something has to define the coordinate space.
static struct surface* toplevel_surface(void) {
  for (int i = 0; i < MAX_SURFACE; i++)
    if (g_surf[i].used && g_surf[i].xdg_toplevel) return &g_surf[i];
  return NULL;
}

// Centre a secondary toplevel over the primary. Wayland deliberately does NOT tell a client
// where its toplevel sits -- placement is the compositor's job -- so unlike an xdg_popup, which
// arrives with a positioner, a dialog carries no position at all and we have to choose one.
// Centring is what a desktop compositor does with a transient, and it keeps the whole surface
// on screen for anything smaller than its parent.
static void secondary_origin(const struct surface* s, int W, int H, int* ox, int* oy) {
  int x = (W - s->rgba_w) / 2, y = (H - s->rgba_h) / 2;
  *ox = x < 0 ? 0 : x;
  *oy = y < 0 ? 0 : y;
}

// ---------------------------------------------------------------------------
// connection
// ---------------------------------------------------------------------------
static int  g_lfd = -1, g_cfd = -1;
static char g_path[256];
static pthread_t g_thread;
static volatile int g_stop;

static uint8_t g_in[1 << 20];
static size_t  g_in_n;
static int     g_rfd[64], g_nrfd;         // fds received via SCM_RIGHTS, in arrival order

static uint32_t g_serial = 1;
static uint32_t next_serial(void) { return g_serial++; }

// Input routing. One surface has focus (there is only ever one), and the client may or may not
// have asked for a pointer/keyboard; events are dropped when it has not.
//
// A CLIENT MAY HOLD MORE THAN ONE wl_pointer, and GTK4 does. Measured from a real
// gtk4-widget-factory run under the headless harness (WLSRV_DEBUG=1, one compositor instance,
// one "client connected", so all of this is one connection):
//     get_registry 2  ... bind wl_seat v5 -> 10 ... get_pointer -> 7  (v5), get_keyboard -> 14
//     get_registry 16 ...
//     get_registry 20 ... bind wl_seat v4 -> 23 ... get_pointer -> 21 (v4)
// Three wl_registry objects on one wl_display and two wl_seat binds. With a single g_ptr_id
// scalar the second get_pointer silently REPLACED the first, so every motion/button/enter from
// then on was addressed to object 21 while GTK4's own seat was still listening on object 7.
//
// wl_pointer is a per-RESOURCE object: the protocol requires an event to reach every resource
// the focused client created, which is why a real compositor keeps a wl_resource list per seat
// rather than one id. Keep a set. Each resource carries its own version (frame is v5+) and its
// own entered state, because they need not have been created at the same moment.
#define MAX_SEATRES 8
struct seatres { uint32_t id, ver; int entered; };
static struct seatres g_ptrs[MAX_SEATRES]; static int g_nptr;
static struct seatres g_kbds[MAX_SEATRES]; static int g_nkbd;
// KEYBOARD focus is the toplevel. POINTER focus is whatever surface the cursor is actually over,
// and once popups are composited those are no longer the same thing: a menu on screen owns the
// pointer wherever it covers the window, while the keyboard stays with the application. They were
// one variable while only one surface could ever be on screen; they cannot be any more.
static uint32_t g_focus_surface;         // keyboard focus: the toplevel (or a role-less surface)
static uint32_t g_toplevel_surface;      // the surface that actually owns the window
static uint32_t g_ptr_surface;           // surface wl_pointer.enter was last sent for (0 = none)
static int32_t  g_ptr_x, g_ptr_y;        // cursor in WINDOW-local coordinates
static uint32_t g_ptr_host_window;       // 0 for primary, otherwise detached toplevel surface
static uint32_t g_interactive_serial, g_interactive_owner;
static int g_interactive_pressed;
static int32_t  g_sent_x = INT32_MIN, g_sent_y = INT32_MIN;   // last position actually published,
                                         // in g_ptr_surface's OWN coordinates
static uint32_t g_mods;

// wl_pointer.set_cursor: the client (Hyprland) advertises a HARDWARE cursor by handing us a
// role-less wl_surface plus a hotspot, and expects the compositor to draw it itself. The surface
// is committed like any other but is not a window or popup, so the ordinary paths never convert
// or blend it — which is why the guest pointer has been invisible. This records the surface and
// hotspot; recomposite() blends the surface over the assembled frame at (g_ptr - hotspot).
static uint32_t g_cursor_surface;        // wl_surface id handed to set_cursor (0 = hidden)
static int32_t  g_cursor_hot_x, g_cursor_hot_y;

// Add (or refresh) a seat resource. Re-creating the same id just updates it.
static void seatres_add(struct seatres* a, int* n, uint32_t id, uint32_t ver) {
  if (!id) return;
  for (int i = 0; i < *n; i++)
    if (a[i].id == id) { a[i].ver = ver; a[i].entered = 0; return; }
  if (*n >= MAX_SEATRES) { wwarn("too many seat resources, dropping %u", id); return; }
  a[*n].id = id; a[*n].ver = ver; a[*n].entered = 0; (*n)++;
}

// A released resource must leave the set, or events keep being addressed to an id the client has
// destroyed — and once the client reuses that id, to the wrong object entirely.
static void seatres_del(struct seatres* a, int* n, uint32_t id) {
  for (int i = 0; i < *n; i++)
    if (a[i].id == id) { a[i] = a[--(*n)]; return; }
}

static void seatres_clear_entered(struct seatres* a, int n) {
  for (int i = 0; i < n; i++) a[i].entered = 0;
}

static uint32_t now_ms(void) {
  struct timeval tv; gettimeofday(&tv, NULL);
  return (uint32_t)(tv.tv_sec * 1000u + tv.tv_usec / 1000u);
}

// --- little-endian accessors (Wayland is host byte order; both ends are LE here) ---
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// --- event builder ---
struct ev { uint8_t b[4096]; size_t n; };
static void ev_begin(struct ev* e, uint32_t id, uint16_t op) {
  e->n = 8; wr32(e->b, id); wr32(e->b + 4, (uint32_t)op);   // size patched in ev_send
}
static void ev_u32(struct ev* e, uint32_t v) {
  if (e->n + 4 > sizeof e->b) return;
  wr32(e->b + e->n, v); e->n += 4;
}
static void ev_str(struct ev* e, const char* s) {
  size_t l = strlen(s) + 1, pad = (4 - (l & 3)) & 3;
  if (e->n + 4 + l + pad > sizeof e->b) return;
  wr32(e->b + e->n, (uint32_t)l); e->n += 4;
  memcpy(e->b + e->n, s, l); e->n += l;
  memset(e->b + e->n, 0, pad); e->n += pad;
}
static void ev_array(struct ev* e, const void* d, size_t l) {
  size_t pad = (4 - (l & 3)) & 3;
  if (e->n + 4 + l + pad > sizeof e->b) return;
  wr32(e->b + e->n, (uint32_t)l); e->n += 4;
  if (l) memcpy(e->b + e->n, d, l);
  e->n += l;
  memset(e->b + e->n, 0, pad); e->n += pad;
}
static void ev_send(struct ev* e) {
  if (g_cfd < 0) return;
  wr32(e->b + 4, (rd32(e->b + 4) & 0xFFFFu) | ((uint32_t)e->n << 16));
  size_t off = 0;
  while (off < e->n) {
    ssize_t w = write(g_cfd, e->b + off, e->n - off);
    if (w < 0) { if (errno == EINTR) continue;
                 if (errno == EAGAIN) { struct pollfd p = { g_cfd, POLLOUT, 0 }; poll(&p, 1, 100); continue; }
                 wwarn("write: %s", strerror(errno)); return; }
    off += (size_t)w;
  }
}
// An event carrying a file descriptor (only wl_keyboard.keymap does).
static void ev_send_fd(struct ev* e, int fd) {
  if (g_cfd < 0) return;
  wr32(e->b + 4, (rd32(e->b + 4) & 0xFFFFu) | ((uint32_t)e->n << 16));
  struct iovec iov = { e->b, e->n };
  union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } c;
  memset(&c, 0, sizeof c);
  struct msghdr mh; memset(&mh, 0, sizeof mh);
  mh.msg_iov = &iov; mh.msg_iovlen = 1;
  mh.msg_control = c.buf; mh.msg_controllen = CMSG_SPACE(sizeof(int));
  struct cmsghdr* cm = CMSG_FIRSTHDR(&mh);
  cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm), &fd, sizeof fd);
  while (sendmsg(g_cfd, &mh, 0) < 0) {
    if (errno == EINTR) continue;
    wwarn("sendmsg(fd): %s", strerror(errno)); return;
  }
}

// wl_display.error / wl_display.delete_id
static void post_error(uint32_t id, uint32_t code, const char* msg) {
  struct ev e; ev_begin(&e, 1, 0); ev_u32(&e, id); ev_u32(&e, code); ev_str(&e, msg); ev_send(&e);
  wwarn("protocol error on %u: %s", id, msg);
}
static void delete_id(uint32_t id) {
  wlog("-> delete_id %u", id);
  struct ev e; ev_begin(&e, 1, 1); ev_u32(&e, id); ev_send(&e);
}

// ---------------------------------------------------------------------------
// globals advertised in the registry
// ---------------------------------------------------------------------------
// Versions are deliberately modest: every version bump adds events the compositor is then
// OBLIGED to send, and a client that binds v6 and never receives, say,
// wl_surface.preferred_buffer_scale is a hang, not a warning.
struct global { uint32_t name; const char* iface; uint32_t version; enum iface kind; };
static const struct global g_globals[] = {
  { 1, "wl_compositor",           4, IF_COMPOSITOR    },
  { 2, "wl_shm",                  1, IF_SHM           },
  { 3, "wl_seat",                 5, IF_SEAT          },
  { 4, "wl_output",               3, IF_OUTPUT        },
  { 5, "xdg_wm_base",             3, IF_WM_BASE       },
  { 6, "wl_subcompositor",        1, IF_SUBCOMPOSITOR },
  { 7, "wl_data_device_manager",  3, IF_DDM           },
  { 8, "org_kde_kwin_server_decoration_manager", 1, IF_SERVER_DECORATION_MANAGER },
  { 9, "zwp_linux_dmabuf_v1",     4, IF_DMABUF        },
  { 10, "wp_single_pixel_buffer_manager_v1", 1, IF_SINGLE_PIXEL_MANAGER },
  { 11, "wp_viewporter",           1, IF_VIEWPORTER     },
  { 12, "zwp_pointer_constraints_v1", 1, IF_POINTER_CONSTRAINTS },
  { 13, "wp_presentation",         1, IF_PRESENTATION   },
};
#define NGLOBALS ((int)(sizeof g_globals / sizeof g_globals[0]))

// wl_shm pixel formats. 0 = ARGB8888, 1 = XRGB8888 — the two Mesa's swrast actually uses.
#define WL_SHM_ARGB8888 0u
#define WL_SHM_XRGB8888 1u

// ---------------------------------------------------------------------------
// presentation
// ---------------------------------------------------------------------------
static int      g_win_open;
static uint64_t g_frames;
// MODE 2 (RDCA=2) state. g_ca_direct is set by surface_commit() for the single call to
// recomposite() that should convert straight from the shm pool into the IOSurface, and cleared
// immediately after; g_ca_top_stale records that the toplevel's retained copy was skipped and
// must be rebuilt before any frame that has to be assembled from more than one surface.
struct wbuffer;
static struct wbuffer* g_ca_direct;
static int             g_ca_top_stale;
static uint32_t g_surf_w, g_surf_h;   // size of the surface currently on screen
static uint8_t* g_rgba;
static size_t   g_rgba_cap;

// Convert one wl_shm buffer into THIS SURFACE's own RGBA8 copy — the format the Metal window
// wants. wl_shm's ARGB8888/XRGB8888 are little-endian 0xAARRGGBB words, i.e. B,G,R,A in memory.
//
// The copy is per-surface rather than one global scratch, because a popup and its toplevel are on
// screen at the same time and each redraws on its own schedule: the retained copies are what let
// recomposite() rebuild the screen from whichever surface did not just damage itself.
// `dst`/`dstride` is where the pixels land. It is s->rgba for every caller today; the CALayer
// path (WLSRV_CALAYER=2) passes an IOSurface's base address instead, which is what removes a
// whole full-frame CPU pass from the single-surface case. dstride is NOT assumed to be width*4:
// an IOSurface's rows are 64-byte aligned, and a packed-stride write into an aligned surface
// shears the image progressively down the screen — which still looks like a working renderer.
// A dma-buf (zwp_linux_dmabuf_v1) buffer has no shm pool behind it: its pixels are the exported
// GBM/DRM BO's host backing (the shim runs the guest in-process). drm_bo_map_by_fd() resolves the
// imported carrier fd's fstat identity back to that backing; we then stride-honour copy it into the
// surface's retained RGBA copy so recomposite() presents it through the ordinary Metal path.
static int convert_dmabuf(struct surface* s, struct wbuffer* b, uint8_t* dst, size_t dstride) {
  size_t sz = 0;
  const uint8_t* src = (const uint8_t*)drm_bo_map_by_fd(b->dmabuf_fd, &sz);
  if (!src || b->width <= 0 || b->height <= 0 || b->stride <= 0) return 0;
  if ((size_t)b->stride < (size_t)b->width * 4) return 0;   // malformed stride
  size_t need = (size_t)b->stride * (size_t)b->height;
  if (need > sz) {
    wwarn("dmabuf %dx%d stride %d exceeds BO (%zu > %zu)",
          b->width, b->height, b->stride, need, sz);
    return 0;
  }
  size_t px = (size_t)b->width * (size_t)b->height * 4;
  if (!dst) {
    if (px > s->rgba_cap) {
      uint8_t* n = realloc(s->rgba, px);
      if (!n) return 0;
      s->rgba = n; s->rgba_cap = px;
    }
    dst = s->rgba; dstride = (size_t)b->width * 4;
  }
  // XR24 (XRGB8888) carries no alpha; whatever is in that byte must not reach the screen as
  // transparency — same rule as the shm path. AR24 (ARGB8888) keeps the client's alpha.
  int has_alpha = (b->format == DRM_FORMAT_ARGB8888);
  for (int y = 0; y < b->height; y++) {
    const uint8_t* sp = src + (size_t)y * (size_t)b->stride;
    uint8_t* d = dst + (size_t)y * dstride;
    for (int x = 0; x < b->width; x++) {
      d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2];       // B,G,R kept as they arrive (BGRA)
      d[3] = has_alpha ? sp[3] : 0xFF;
      sp += 4; d += 4;
    }
  }
  s->rgba_w = b->width; s->rgba_h = b->height;
  static unsigned presented;
  if (++presented <= 3 || presented % 100 == 0)
    fprintf(stderr, "WLSRV_DMABUF_PRESENT frame=%u fd=%d size=%dx%d stride=%d format=0x%08x\n",
            presented, b->dmabuf_fd, b->width, b->height, b->stride, b->format);
  return 1;
}

static int convert_buffer_to(struct surface* s, struct wbuffer* b, uint8_t* dst, size_t dstride) {
  if (b->dmabuf) return convert_dmabuf(s, b, dst, dstride);
  if (b->pool < 0 || b->pool >= MAX_POOL || !g_pool[b->pool].used) return 0;
  struct shmpool* p = &g_pool[b->pool];
  if (b->width <= 0 || b->height <= 0 || b->stride <= 0) return 0;

  size_t need = (size_t)b->offset + (size_t)b->stride * (size_t)b->height;
  if (need > p->size) { wwarn("buffer %dx%d stride %d exceeds pool (%zu > %zu)",
                              b->width, b->height, b->stride, need, p->size); return 0; }

  size_t px = (size_t)b->width * (size_t)b->height * 4;
  if (!dst) {
    if (px > s->rgba_cap) {
      uint8_t* n = realloc(s->rgba, px);
      if (!n) return 0;
      s->rgba = n; s->rgba_cap = px;
    }
    dst = s->rgba; dstride = (size_t)b->width * 4;
  }
  const uint8_t* src = p->map + b->offset;
  uint64_t _t = prof_t0();

  // THE CHEAPEST WORK IS WORK YOU DELETE. This used to swizzle B,G,R,A into R,G,B,A one byte at a
  // time — 0.94 ms per 1024x1024 frame, 3.26 ms at 2048x2048 (MEASURED) — for one reason only:
  // window.m's staging texture was MTLPixelFormatRGBA8Unorm. But wl_shm's ARGB8888/XRGB8888 IS
  // B,G,R,A in memory, the CAMetalLayer drawable is BGRA8Unorm already, and Metal has a native
  // MTLPixelFormatBGRA8Unorm that samples B,G,R,A for free. So the compositor now keeps the
  // client's own byte order all the way to the GPU and tells the window which order it is
  // (x11rnd_present_bgra), and the swizzle is not accelerated — it is GONE.
  //
  // What remains is a stride-honouring copy, which is vImageCopyBuffer's whole job, plus one thing
  // Metal cannot do for us: XRGB8888 carries NO alpha, and whatever is in that byte must not reach
  // the screen as transparency. vImageOverwriteChannelsWithScalar_ARGB8888's mask is
  // most-significant-bit-first over the four bytes, so the LAST byte is 0x1 — asserted, not
  // assumed, by wlsrv_convert_selftest(), because an inverted mask writes 0xFF into the FIRST byte
  // and on an opaque window that is invisible.
  //
  // The copy itself is still not free (0.12 ms/Mpx) and in the common single-surface, packed-stride
  // case it is avoidable too: the shm mapping could go straight to replaceRegion, since the staging
  // upload completes before wl_buffer.release is sent. Not done here — it needs the retained-copy
  // invariant that lets recomposite() rebuild the screen when a popup redraws alone, and P3 has
  // just made release timing load-bearing on their side. Measured value if someone takes it:
  // ~0.12 ms/frame at 1024x1024, ~0.5 ms at 2048x2048.
  const vImage_Buffer vs = { (void*)src,       (vImagePixelCount)b->height,
                             (vImagePixelCount)b->width, (size_t)b->stride };
  const vImage_Buffer vd = { dst,              (vImagePixelCount)b->height,
                             (vImagePixelCount)b->width, dstride };
  vImage_Error ve = scalar_forced() ? kvImageInternalError
                                    : vImageCopyBuffer(&vs, &vd, 4, vimg_flags_px(px / 4));
  if (ve != kvImageNoError) {                        // never seen; fall back rather than blank
    for (int y = 0; y < b->height; y++) {
      const uint8_t* sp = src + (size_t)y * (size_t)b->stride;
      uint8_t* d = dst + (size_t)y * dstride;
      for (int x = 0; x < b->width; x++) {
        d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2];
        d[3] = (b->format == WL_SHM_ARGB8888) ? sp[3] : 0xFF;
        sp += 4; d += 4;
      }
    }
  } else if (b->format != WL_SHM_ARGB8888) {
    vImageOverwriteChannelsWithScalar_ARGB8888(0xFF, &vd, &vd, 0x1, vimg_flags_px(px / 4));
  }
  prof_add(PH_CONVERT, _t);
  s->rgba_w = b->width; s->rgba_h = b->height;
  return 1;
}

static int convert_buffer(struct surface* s, struct wbuffer* b) {
  return convert_buffer_to(s, b, NULL, 0);
}

// wp_single_pixel_buffer_v1: the buffer IS one colour and carries no dimensions of its own. When
// it is attached the surface keeps its logical size — KWin sets that with wp_viewport.set_destination
// (1024x768 for its nested output) — so the fill is sized by vp_w/vp_h, falling back to the last
// buffer size the surface carried. `packed` is (a<<24)|(r<<16)|(g<<8)|b, which in memory order is
// exactly B,G,R,A — the byte order recomposite() presents.
static void fill_single_pixel(struct surface* s, uint32_t packed) {
  int w = s->vp_w > 0 ? s->vp_w : s->rgba_w;
  int h = s->vp_h > 0 ? s->vp_h : s->rgba_h;
  if (w <= 0 || h <= 0) return;                 // no size yet: nothing meaningful to fill
  size_t px = (size_t)w * (size_t)h * 4;
  if (px > s->rgba_cap) {
    uint8_t* n = realloc(s->rgba, px);
    if (!n) return;
    s->rgba = n; s->rgba_cap = px;
  }
  const uint8_t b = (uint8_t)(packed & 0xff);
  const uint8_t g = (uint8_t)((packed >> 8) & 0xff);
  const uint8_t r = (uint8_t)((packed >> 16) & 0xff);
  const uint8_t a = (uint8_t)((packed >> 24) & 0xff);
  for (size_t i = 0; i < px; i += 4) {
    s->rgba[i] = b; s->rgba[i + 1] = g; s->rgba[i + 2] = r; s->rgba[i + 3] = a;
  }
  s->rgba_w = w; s->rgba_h = h;
  wlog("single-pixel fill %dx%d color r=%u g=%u b=%u a=%u", w, h, r, g, b, a);
}

// Absolute top-left of a subsurface in TOPLEVEL-local coordinates, by walking the subsurface
// parent chain (each link adds its set_position). KWin's layer is flat, but a nested chain costs
// nothing to resolve correctly.
static void subsurface_origin(struct surface* s, int* ox, int* oy) {
  int x = 0, y = 0, guard = 0;
  struct surface* cur = s;
  while (cur && cur->is_subsurface && guard++ < MAX_SURFACE) {
    x += cur->subs_x; y += cur->subs_y;
    cur = surface_of(cur->subsurface_parent);
  }
  *ox = x; *oy = y;
}

// wl_subsurface.place_above / place_below: re-insert `self` adjacent to `sibling` among the
// subsurfaces of their shared parent, then renumber so the ordering is dense and deterministic.
static void place_subsurface_above(struct surface* self, struct surface* sibling) {
  if (!self || !sibling || self == sibling || !self->is_subsurface) return;
  if (self->subsurface_parent != sibling->subsurface_parent) return;
  struct surface* order[MAX_SURFACE];
  int n = 0;
  for (int i = 0; i < MAX_SURFACE; i++) {
    struct surface* s = &g_surf[i];
    if (!s->used || !s->is_subsurface || s == self) continue;
    if (s->subsurface_parent != self->subsurface_parent) continue;
    order[n++] = s;
  }
  for (int i = 1; i < n; i++) {
    struct surface* key = order[i];
    int j = i - 1;
    while (j >= 0 && order[j]->subs_z > key->subs_z) { order[j + 1] = order[j]; j--; }
    order[j + 1] = key;
  }
  struct surface* fin[MAX_SURFACE];
  int m = 0, inserted = 0;
  for (int i = 0; i < n; i++) {
    fin[m++] = order[i];
    if (order[i] == sibling) { fin[m++] = self; inserted = 1; }
  }
  if (!inserted) fin[m++] = self;              // sibling vanished: self goes on top
  for (int i = 0; i < m; i++) fin[i]->subs_z = (uint32_t)(i + 1);
}
static void place_subsurface_below(struct surface* self, struct surface* sibling) {
  if (!self || !sibling || self == sibling || !self->is_subsurface) return;
  if (self->subsurface_parent != sibling->subsurface_parent) return;
  struct surface* order[MAX_SURFACE];
  int n = 0;
  for (int i = 0; i < MAX_SURFACE; i++) {
    struct surface* s = &g_surf[i];
    if (!s->used || !s->is_subsurface || s == self) continue;
    if (s->subsurface_parent != self->subsurface_parent) continue;
    order[n++] = s;
  }
  for (int i = 1; i < n; i++) {
    struct surface* key = order[i];
    int j = i - 1;
    while (j >= 0 && order[j]->subs_z > key->subs_z) { order[j + 1] = order[j]; j--; }
    order[j + 1] = key;
  }
  struct surface* fin[MAX_SURFACE];
  int m = 0, inserted = 0;
  for (int i = 0; i < n; i++) {
    if (order[i] == sibling && !inserted) { fin[m++] = self; inserted = 1; }
    fin[m++] = order[i];
  }
  if (!inserted) fin[m++] = self;
  for (int i = 0; i < m; i++) fin[i]->subs_z = (uint32_t)(i + 1);
}

// A self-test for the two vImage calls above, run once at compositor start when WLSRV_SELFTEST=1.
//
// It exists because BOTH of those calls have a silent-wrong-answer failure mode that looks like
// success: a reversed permute map still produces a plausible image (red and blue swapped is not
// obviously wrong on a blue-grey window), and an inverted overwrite mask writes 0xFF into RED
// instead of alpha, which on an opaque surface is invisible. So the reference here is the scalar
// loop that used to BE the implementation, compared byte for byte on a pattern that makes every
// channel distinguishable, for both formats.
static void wlsrv_convert_selftest(void) {
  static const int widths[]  = { 1, 3, 7, 16, 17, 37, 64, 255, 256 };
  static const int heights[] = { 1, 2, 5, 33 };
  static const int pads[]    = { 0, 4, 64 };     // stride slack: real pools are not always packed
  enum { MAXW = 256, MAXH = 33, MAXPAD = 64 };
  static uint8_t in[(MAXW * 4 + MAXPAD) * MAXH], want[MAXW * MAXH * 4], got[MAXW * MAXH * 4];
  static uint8_t rgb_want[MAXW * MAXH * 3], rgb_got[MAXW * MAXH * 3];
  int bad = 0, cases = 0;
  uint32_t seed = 0x1234567u;
#define NEXTB() ((uint8_t)((seed = seed * 1664525u + 1013904223u) >> 19))

  for (unsigned wi = 0; wi < sizeof widths / sizeof *widths; wi++)
  for (unsigned hi = 0; hi < sizeof heights / sizeof *heights; hi++)
  for (unsigned pi = 0; pi < sizeof pads / sizeof *pads; pi++)
  for (int fmt = 0; fmt < 2; fmt++) {            // 0 = ARGB8888, 1 = XRGB8888
    int W = widths[wi], H = heights[hi], stride = W * 4 + pads[pi];
    cases++;
    for (int i = 0; i < stride * H; i++) in[i] = NEXTB();
    // THE REFERENCE IS THE FRAMEBUFFER THE OLD CODE PRODUCED, expressed in the new byte order —
    // not a re-derivation of what any of it was supposed to do. Old byte k of the RGBA framebuffer
    // is new byte 2-k of the BGRA one for k<3, so "the pixels did not change" is a statement about
    // what reaches the SCREEN, and the end-to-end check of that is the PPM: it is R,G,B in both
    // worlds, so its bytes must be identical before and after. That is asserted below and by
    // sha256 over real dumped frames.
    for (int y = 0; y < H; y++) {
      const uint8_t* sp = in + (size_t)y * (size_t)stride;
      uint8_t* d = want + (size_t)y * (size_t)W * 4;
      for (int x = 0; x < W; x++) {
        d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2];          // B,G,R kept as they arrived
        d[3] = (fmt == 0) ? sp[3] : 0xFF;                  // XRGB has no alpha
        sp += 4; d += 4;
      }
    }
    memset(got, 0xAA, (size_t)W * (size_t)H * 4);
    const vImage_Buffer vs = { in,  (vImagePixelCount)H, (vImagePixelCount)W, (size_t)stride };
    const vImage_Buffer vd = { got, (vImagePixelCount)H, (vImagePixelCount)W, (size_t)W * 4 };
    vImageCopyBuffer(&vs, &vd, 4, kvImageNoFlags);
    if (fmt != 0) vImageOverwriteChannelsWithScalar_ARGB8888(0xFF, &vd, &vd, 0x1, kvImageNoFlags);
    if (memcmp(want, got, (size_t)W * (size_t)H * 4) != 0) {
      for (int i = 0; i < W * H * 4; i++)
        if (want[i] != got[i]) {
          fprintf(stderr, "[wl] SELFTEST FAIL convert %dx%d stride %d fmt %d: byte %d "
                          "want %02x got %02x\n", W, H, stride, fmt, i, want[i], got[i]);
          break;
        }
      bad = 1;
    }

    // THE END-TO-END ASSERTION: the PPM bytes must be what the ORIGINAL code would have written.
    // The original wrote framebuffer bytes 0,1,2 of an RGBA framebuffer, i.e. source R,G,B — which
    // in the source buffer are bytes 2,1,0. If BGRA end-to-end had gone in with red and blue
    // crossed anywhere, this is where it shows, because a PPM is R,G,B in both worlds.
    for (int y = 0; y < H; y++) {
      const uint8_t* sp = in + (size_t)y * (size_t)stride;
      uint8_t* d = rgb_want + (size_t)y * (size_t)W * 3;
      for (int x = 0; x < W; x++) { d[0] = sp[2]; d[1] = sp[1]; d[2] = sp[0]; sp += 4; d += 3; }
    }
    memset(rgb_got, 0x55, (size_t)W * (size_t)H * 3);
    const vImage_Buffer s4 = { got,     (vImagePixelCount)H, (vImagePixelCount)W, (size_t)W * 4 };
    const vImage_Buffer d3 = { rgb_got, (vImagePixelCount)H, (vImagePixelCount)W, (size_t)W * 3 };
    if (vImageConvert_BGRA8888toRGB888(&s4, &d3, kvImageNoFlags) != kvImageNoError ||
        memcmp(rgb_want, rgb_got, (size_t)W * (size_t)H * 3) != 0) {
      fprintf(stderr, "[wl] SELFTEST FAIL dump-pack %dx%d stride %d\n", W, H, stride); bad = 1;
    }
  }

  // NEGATIVE CONTROLS. A comparison that cannot fail proves nothing, and this project's whole
  // history is instruments that agreed with everything. Each of these must come out MISMATCHED.
  {
    enum { W = 37, H = 11 };
    for (int i = 0; i < W * H * 4; i++) in[i] = NEXTB();
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        const uint8_t* sp = in + ((size_t)y * W + x) * 4;
        uint8_t* d = want + ((size_t)y * W + x) * 4;
        // The CORRECT answer under BGRA end-to-end: colour bytes pass through untouched.
        // (This line said sp[2],sp[1],sp[0] on the first attempt — the pre-BGRA expectation left
        // behind — which made control (a) below compare EQUAL and report the instrument blind.
        // It was not blind; it was right, and it caught the stale reference. Left on the record
        // because a negative control that has never gone red is not yet a control, and this one
        // went red on its author within the minute.)
        d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2]; d[3] = 0xFF;
      }
    const vImage_Buffer vs = { in,  H, W, W * 4 };
    const vImage_Buffer vd = { got, H, W, W * 4 };
    // (a) A RED/BLUE SWAP MUST BE VISIBLE TO THIS TEST. This is the exact mutation "BGRA end to
    // end" invites, it is the one a human eye does not reliably catch on a blue-grey window, and
    // if the checker cannot see it then nothing above is evidence of anything.
    static const uint8_t swapped[4] = { 2, 1, 0, 3 };
    vImagePermuteChannels_ARGB8888(&vs, &vd, swapped, kvImageNoFlags);
    vImageOverwriteChannelsWithScalar_ARGB8888(0xFF, &vd, &vd, 0x1, kvImageNoFlags);
    if (memcmp(want, got, (size_t)W * H * 4) == 0) {
      fprintf(stderr, "[wl] SELFTEST FAIL: red and blue crossed and the comparison said EQUAL — "
                      "it is blind, so every pass above means nothing\n");
      bad = 1;
    }
    // (b) The mask: 0x8 addresses the FIRST byte, not the last. If this compared equal, the
    // overwrite is a no-op and the XRGB alpha assertions above are vacuous.
    vImageCopyBuffer(&vs, &vd, 4, kvImageNoFlags);
    vImageOverwriteChannelsWithScalar_ARGB8888(0xFF, &vd, &vd, 0x8, kvImageNoFlags);
    if (memcmp(want, got, (size_t)W * H * 4) == 0) {
      fprintf(stderr, "[wl] SELFTEST FAIL: mask 0x8 also produced the right answer, so the mask "
                      "is not what sets alpha and 0x1 was believed for the wrong reason\n");
      bad = 1;
    }
    // (c) And the PPM pack must not be blind either: BGRA->RGB and RGBA->RGB differ by exactly the
    // swap under test, so feeding the same bytes through the wrong one must NOT agree.
    vImageCopyBuffer(&vs, &vd, 4, kvImageNoFlags);
    for (int i = 0; i < W * H; i++) {
      rgb_want[i * 3 + 0] = got[i * 4 + 2];
      rgb_want[i * 3 + 1] = got[i * 4 + 1];
      rgb_want[i * 3 + 2] = got[i * 4 + 0];
    }
    const vImage_Buffer s4n = { got,     H, W, W * 4 };
    const vImage_Buffer d3n = { rgb_got, H, W, W * 3 };
    vImageConvert_RGBA8888toRGB888(&s4n, &d3n, kvImageNoFlags);
    if (memcmp(rgb_want, rgb_got, (size_t)W * H * 3) == 0) {
      fprintf(stderr, "[wl] SELFTEST FAIL: RGBA->RGB and BGRA->RGB agreed, so the dump pack "
                      "cannot tell the two byte orders apart\n");
      bad = 1;
    }
  }
#undef NEXTB
  fprintf(stderr, "[wl] convert selftest: %s (%d cases, widths 1..256, strides packed and "
                  "padded, ARGB + XRGB, dump pack, 3 negative controls)\n",
          bad ? "FAIL" : "ok", cases);
}

// The PPM writer's old inner loop, kept as the fallback and as WLSRV_SCALAR=1's arm. A PPM row is
// R,G,B and the framebuffer is B,G,R,A, so the indices are 2,1,0 — the same swap that used to be
// paid once per pixel on EVERY frame is now paid only when someone is dumping.
static void dump_rows_scalar(FILE* f, const uint8_t* fb, size_t stride, int W, int H) {
  for (int y = 0; y < H; y++) {
    const uint8_t* row = fb + (size_t)y * stride;
    for (int x = 0; x < W; x++) {
      uint8_t rgb[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0] };
      fwrite(rgb, 1, 3, f);
    }
  }
}

// Draw the toplevel, then blend every mapped popup over it, and present the result.
//
// Presenting a popup INSTEAD of the toplevel — which a single global framebuffer amounts to —
// resizes the host window down to the menu and rewrites the rectangle pointer hit-testing uses.
// Presenting only the toplevel, which is what the is_window test alone gives you, is the opposite
// failure and the one that is live today: the menu is never drawn at all, so GTK4 opens a popover
// that is fully functional and completely invisible.
static void recomposite(void) {
  uint64_t _tf = prof_t0();
  // THE CLIENT'S FRAME PERIOD, seen from the compositor: the gap between successive presents.
  // C1's in-ICD timer already reports this distribution, but ONLY for clients that go through
  // that ICD — which excludes llvmpipe, every GTK/Qt shm client, and any future backend. One
  // recomposite() happens per present, so the interval here is the same quantity for ANY
  // client, and it is what lets a p50/p95 claim be made about the software path at all.
  if (prof_on()) {
    static uint64_t prev;
    uint64_t now = mach_absolute_time();
    if (prev) prof_add(PH_PERIOD, prev);   // prof_add measures now - prev, which is the gap
    prev = now;
  }
  struct surface* top = toplevel_surface();
  // g_ca_direct is the toplevel's shm buffer when mode 2 has decided this frame goes straight
  // from the pool into the IOSurface. In that case top->rgba may not exist at all — that is the
  // whole point — so it is not required here.
  if (!top || (!top->rgba && !g_ca_direct) || top->rgba_w <= 0 || top->rgba_h <= 0) return;
  int W = top->rgba_w, H = top->rgba_h;

  // WHERE THIS FRAME IS ASSEMBLED. On the Metal path it is g_rgba, a malloc'd framebuffer that
  // then has to be uploaded into a staging texture. On the CA path it is the IOSurface itself,
  // so the assembly IS the upload: WindowServer reads these very bytes. That is the one
  // structural saving this whole exercise is about, so it happens here, in the open, rather than
  // behind a present() call that could be doing anything.
  //
  // The window has to exist before a surface can be acquired (the layer is created with it), so
  // x11rnd_open moved ABOVE the acquire. On the Metal path that is a no-op reorder — it is
  // idempotent and was already being called every frame.
  if (!g_win_open || g_surf_w != (uint32_t)W || g_surf_h != (uint32_t)H) {
    x11rnd_open((uint32_t)W, (uint32_t)H);
    g_win_open = 1;
    g_surf_w = (uint32_t)W; g_surf_h = (uint32_t)H;
  }

  uint8_t* fb = NULL;
  size_t   fbstride = (size_t)W * 4;
  int      on_ca = 0;
  if (capres_enabled()) {
    fb = capres_acquire((uint32_t)W, (uint32_t)H, &fbstride);
    on_ca = (fb != NULL);
    if (!on_ca) fbstride = (size_t)W * 4;            // acquire failed: fall back, say so once
  }
  size_t px = (size_t)W * (size_t)H * 4;
  if (!on_ca) {
    if (px > g_rgba_cap) {
      uint8_t* n = realloc(g_rgba, px);
      if (!n) return;
      g_rgba = n; g_rgba_cap = px;
    }
    fb = g_rgba;
  }
  if (on_ca && g_ca_direct) {
    // MODE 2: the shm pool goes STRAIGHT into the IOSurface. There is no per-surface retained
    // copy and no assembly buffer, so the "copy" phase does not happen — and prof_report() now
    // prints it as a row saying it did not execute rather than omitting it.
    //
    // Nothing else is mapped (ca_direct_ok() checked that), so the blend loops below are no-ops
    // by construction; they are left in place rather than branched around, because a second code
    // path through the compositing loop is exactly how a popup stops being drawn.
    if (!convert_buffer_to(top, g_ca_direct, fb, fbstride)) { capres_publish(); return; }
    g_ca_top_stale = 1;                 // top->rgba is now behind the screen; see the repair below
  } else {
    // The toplevel's retained copy has to exist and be current. Under mode 2 it can be stale —
    // the direct path skipped it — and the first frame after a popup maps is exactly when it is
    // needed again. Repair it from the last surface we published, which IS that missing frame.
    if (g_ca_top_stale) {
      size_t lst = 0;
      const uint8_t* last = capres_last_published((uint32_t)W, (uint32_t)H, &lst);
      if (last && top->rgba && px <= top->rgba_cap) {
        const vImage_Buffer rs = { (void*)last, (vImagePixelCount)H, (vImagePixelCount)W, lst };
        const vImage_Buffer rd = { top->rgba,   (vImagePixelCount)H, (vImagePixelCount)W, (size_t)W * 4 };
        vImageCopyBuffer(&rs, &rd, 4, kvImageDoNotTile);
        wlog("mode 2: rebuilt the toplevel's retained copy from the last published IOSurface "
             "(a second surface is now mapped, so frames need assembling again)");
      }
      g_ca_top_stale = 0;
    }
    uint64_t _tc = prof_t0();
    // Stride-honouring, because an IOSurface row is 64-byte aligned and top->rgba's is packed.
    // vImageCopyBuffer is the same call convert_buffer uses and degenerates to the old memcpy when
    // the two strides match, which is every frame on the Metal path.
    if (fbstride == (size_t)W * 4) {
      memcpy(fb, top->rgba, px);
    } else {
      const vImage_Buffer cs = { top->rgba, (vImagePixelCount)H, (vImagePixelCount)W, (size_t)W * 4 };
      const vImage_Buffer cd = { fb,        (vImagePixelCount)H, (vImagePixelCount)W, fbstride };
      vImageCopyBuffer(&cs, &cd, 4, vimg_flags_px((size_t)W * (size_t)H));
    }
    prof_add(PH_COPY, _tc);
  }

  // SUBSURFACES (wl_subcompositor), composited over the parent surface's own content in z-order.
  //
  // KWin's nested Wayland output draws its desktop as a single-pixel background on the main
  // toplevel plus a flat layer of wl_subsurfaces for the actual content (background, panel,
  // plasma shell). Each is placed at (parent position + set_position) — resolved to toplevel
  // coordinates by walking the parent chain — and blended with the same straight-alpha "over"
  // used for popups and the cursor.
  for (uint32_t pass = 1; pass <= MAX_SURFACE; pass++) {
    for (int i = 0; i < MAX_SURFACE; i++) {
      struct surface* s = &g_surf[i];
      if (!s->used || !s->is_subsurface || !s->mapped || s->subs_z != pass) continue;
      if (!s->rgba || s->rgba_w <= 0 || s->rgba_h <= 0) continue;
      int ox, oy; subsurface_origin(s, &ox, &oy);
      for (int y = 0; y < s->rgba_h; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= H) continue;
        const uint8_t* sp = s->rgba + (size_t)y * (size_t)s->rgba_w * 4;
        for (int x = 0; x < s->rgba_w; x++) {
          int dx = ox + x;
          if (dx < 0 || dx >= W) { sp += 4; continue; }
          uint8_t* d = fb + (size_t)dy * fbstride + (size_t)dx * 4;
          unsigned al = sp[3];
          if (al == 255) { d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2]; d[3] = 255; }
          else if (al) {                                   // straight-alpha "over"
            d[0] = (uint8_t)((sp[0] * al + d[0] * (255 - al)) / 255);
            d[1] = (uint8_t)((sp[1] * al + d[1] * (255 - al)) / 255);
            d[2] = (uint8_t)((sp[2] * al + d[2] * (255 - al)) / 255);
            d[3] = 255;
          }
          sp += 4;
        }
      }
    }
  }
  uint64_t _tb = prof_t0();

  // SECONDARY TOPLEVELS (dialogs), under the popups but over the primary.
  //
  // Compositing only the primary toplevel -- which is what a single toplevel_surface() lookup
  // gives you -- is the same class of failure that made GTK4 popovers invisible, and it is worse
  // in one specific way: a modal dialog is INVISIBLE while the toolkit entirely correctly refuses
  // input to the window behind it. The app then looks frozen and unresponsive to every click,
  // which reads as a broken input bridge rather than a missing surface.
  //
  // R2 proved that shape on Qt Designer: it opens a "New Form" dialog at startup as a second
  // xdg_toplevel, rendered 7 byte-identical frames, and ignored every click -- while Qt's own QPA
  // log showed it RECEIVING the Enter and MouseButtonPress. Passing a .ui file skips the dialog,
  // leaves one toplevel, and the File menu opens normally. Their tests/qt/qt2win.cpp reduces it,
  // and splits it cleanly: input routing to a second toplevel is FINE (window A's button still
  // fires) -- it is presentation that was missing.
  for (int i = 0; i < MAX_SURFACE; i++) {
    struct surface* s = &g_surf[i];
    if (!s->used || !s->xdg_toplevel || s == top || !s->mapped) continue;
    if (multiwindow_enabled()) continue;  // this toplevel has its own NSWindow
    if (!s->rgba || s->rgba_w <= 0 || s->rgba_h <= 0) continue;
    int ox, oy; secondary_origin(s, W, H, &ox, &oy);
    s->px = ox; s->py = oy;                 // publish it: hit-testing reads px/py
    for (int y = 0; y < s->rgba_h; y++) {
      int dy = oy + y;
      if (dy < 0 || dy >= H) continue;
      const uint8_t* sp = s->rgba + (size_t)y * (size_t)s->rgba_w * 4;
      for (int x = 0; x < s->rgba_w; x++) {
        int dx = ox + x;
        if (dx < 0 || dx >= W) { sp += 4; continue; }
        uint8_t* d = fb + (size_t)dy * fbstride + (size_t)dx * 4;
        unsigned al = sp[3];
        if (al == 255) { d[0]=sp[0]; d[1]=sp[1]; d[2]=sp[2]; d[3]=255; }
        else if (al) {
          d[0]=(uint8_t)((sp[0]*al + d[0]*(255-al))/255);
          d[1]=(uint8_t)((sp[1]*al + d[1]*(255-al))/255);
          d[2]=(uint8_t)((sp[2]*al + d[2]*(255-al))/255);
          d[3]=255;
        }
        sp += 4;
      }
    }
  }

  // Popups in creation order: a later one is on top of an earlier one, which is what puts a
  // submenu over the menu that opened it.
  for (uint32_t pass = 1; pass < g_stack_next; pass++) {
    for (int i = 0; i < MAX_SURFACE; i++) {
      struct surface* s = &g_surf[i];
      if (!s->used || !s->xdg_popup || !s->mapped || s->stack != pass) continue;
      if (multiwindow_enabled() && top && s->popup_owner && s->popup_owner != top->id) continue;
      if (!s->rgba || s->rgba_w <= 0 || s->rgba_h <= 0) continue;
      for (int y = 0; y < s->rgba_h; y++) {
        int dy = s->py + y;
        if (dy < 0 || dy >= H) continue;
        const uint8_t* sp = s->rgba + (size_t)y * (size_t)s->rgba_w * 4;
        for (int x = 0; x < s->rgba_w; x++) {
          int dx = s->px + x;
          if (dx < 0 || dx >= W) { sp += 4; continue; }
          uint8_t* d = fb + (size_t)dy * fbstride + (size_t)dx * 4;
          unsigned al = sp[3];
          if (al == 255) { d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2]; d[3] = 255; }
          else if (al) {                                     // straight-alpha "over"
            d[0] = (uint8_t)((sp[0] * al + d[0] * (255 - al)) / 255);
            d[1] = (uint8_t)((sp[1] * al + d[1] * (255 - al)) / 255);
            d[2] = (uint8_t)((sp[2] * al + d[2] * (255 - al)) / 255);
            d[3] = 255;
          }
          sp += 4;
        }
      }
    }
  }

  // THE GUEST CURSOR. Hyprland advertises a hardware cursor via wl_pointer.set_cursor: a
  // role-less wl_surface holding the cursor image plus a hotspot, which this compositor is
  // expected to draw. It never goes through the window/popup paths above, so without this the
  // pointer is invisible. Blend it over the assembled frame at (g_ptr - hotspot), top-left
  // origin, with the same straight-alpha "over" as popups. g_ptr_x/y and `fb` are both in the
  // toplevel framebuffer's pixel space (see gpuwin_cursor()), so no further transform is needed.
  {
    struct surface* cur = surface_of(g_cursor_surface);
    if (cur && cur->rgba && cur->rgba_w > 0 && cur->rgba_h > 0) {
      int ox = g_ptr_x - g_cursor_hot_x;
      int oy = g_ptr_y - g_cursor_hot_y;
      for (int y = 0; y < cur->rgba_h; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= H) continue;
        const uint8_t* sp = cur->rgba + (size_t)y * (size_t)cur->rgba_w * 4;
        for (int x = 0; x < cur->rgba_w; x++) {
          int dx = ox + x;
          if (dx < 0 || dx >= W) { sp += 4; continue; }
          uint8_t* d = fb + (size_t)dy * fbstride + (size_t)dx * 4;
          unsigned al = sp[3];
          if (al == 255) { d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2]; d[3] = 255; }
          else if (al) {                                     // straight-alpha "over"
            d[0] = (uint8_t)((sp[0] * al + d[0] * (255 - al)) / 255);
            d[1] = (uint8_t)((sp[1] * al + d[1] * (255 - al)) / 255);
            d[2] = (uint8_t)((sp[2] * al + d[2] * (255 - al)) / 255);
            d[3] = 255;
          }
          sp += 4;
        }
      }
    }
  }

  prof_add(PH_BLEND, _tb);

  // (The window-size tracking that used to live here moved above the framebuffer acquire: the
  // layer the CA path publishes into is created with the window, so the window has to exist
  // first. It is idempotent and was already run every frame.)
  uint64_t _tp = prof_t0();
  if (on_ca) {
    // No staging texture, no render pass, no nextDrawable: the bytes just composited ARE the
    // ones WindowServer will scan out. This call unlocks the surface and assigns it to the layer.
    capres_publish();
  } else {
    // BGRA, and we say so. See x11srv.h: the X11 server's framebuffer really is RGBA, so the byte
    // order travels with the frame instead of being a global setting somebody has to remember.
    x11rnd_present_bgra(fb, (uint32_t)W, (uint32_t)H);
  }
  prof_add(PH_PRESENT, _tp);
  uint64_t _td = prof_t0();

  // Low-overhead, opt-in timing probe for the owned 320x240 bridge fixture.
  // It samples a single BGRA pixel after producer present, not physical scanout.
  if (getenv("WLSRV_COLOR_TRACE") && W > 100 && H > 100) {
    static int last_color = -1;
    const char* xs = getenv("WLSRV_COLOR_TRACE_X");
    const char* ys = getenv("WLSRV_COLOR_TRACE_Y");
    const int x = xs ? atoi(xs) : 100, y = ys ? atoi(ys) : 100;
    if (x < 0 || y < 0 || x >= W || y >= H) goto color_trace_done;
    const uint8_t* p = (const uint8_t*)fb + (size_t)y * fbstride + (size_t)x * 4;
    int color = p[2] > 150 && p[1] < 120 ? 1 :
                p[0] > 150 && p[2] < 120 ? 2 : 0;
    if (color != last_color) {
      struct timespec stamped;
      clock_gettime(CLOCK_UPTIME_RAW, &stamped);
      uint64_t ns = (uint64_t)stamped.tv_sec * 1000000000ull + (uint64_t)stamped.tv_nsec;
      fprintf(stderr, "[wl-color] uptime_ns=%llu state=%d frame=%llu\n",
              (unsigned long long)ns, color, (unsigned long long)g_frames);
      last_color = color;
    }
color_trace_done:;
  }

  // WLSRV_DUMP=<prefix> writes presented frames as binary PPMs, so the pixels can be checked
  // independently of whether anything is visible on screen.
  //
  // The window is ROLLING, not the first N. Keeping the first N captures a toolkit's startup
  // paint and nothing else, which is exactly the wrong half: input arrives seconds later, and a
  // Wayland client only redraws on damage, so the frames that show whether a click did anything
  // are the LAST ones. Slot index is frame % N and the frame number goes into a PPM comment, so
  // "which frame is this" survives the wrap. WLSRV_DUMP_MAX resizes the window.
  const char* dump = getenv("WLSRV_DUMP");
  if (dump) {
    static int cap = -1;
    if (cap < 0) { const char* m = getenv("WLSRV_DUMP_MAX"); cap = (m && atoi(m) > 0) ? atoi(m) : 8; }
    char path[512];
    snprintf(path, sizeof path, "%s%03d.ppm", dump, (int)(g_frames % (uint64_t)cap));
    // ONE THREE-BYTE fwrite PER PIXEL was 24.7 ms per 1024x1024 frame — MEASURED min, p50 27.0 —
    // and that is not a footnote: it is the single largest cost in the whole GPU stack, it is paid
    // by every gate that dumps (c1-gate, tiled_gate, qt-selftest), and it was being read as
    // "compositor back-pressure" because it lives inside recomposite(). Turning it off takes
    // 1024x1024 llvmpipe from 30 fps to 117 fps.
    //
    // vImageConvert_BGRA8888toRGB888 does the 4->3 pack AND the B/R swap in one vectorised pass
    // (the framebuffer is BGRA now; a PPM is R,G,B), and the result goes out in ONE write. The
    // BYTES ARE IDENTICAL to what the old loop produced — asserted by sha256 over the dumped
    // frames and by wlsrv_convert_selftest(), not by reading the code.
    FILE* f = fopen(path, "wb");
    if (f) {
      if (getenv("WLSRV_DUMP_TS")) {
        struct timespec stamped;
        clock_gettime(CLOCK_UPTIME_RAW, &stamped);
        uint64_t ns = (uint64_t)stamped.tv_sec * 1000000000ull + (uint64_t)stamped.tv_nsec;
        fprintf(f, "P6\n# frame %llu uptime_ns %llu\n%d %d\n255\n",
                (unsigned long long)g_frames, (unsigned long long)ns, W, H);
      } else {
        fprintf(f, "P6\n# frame %llu\n%d %d\n255\n", (unsigned long long)g_frames, W, H);
      }
      static uint8_t* rgb; static size_t rgb_cap;
      size_t need = (size_t)W * (size_t)H * 3;
      if (need > rgb_cap) { uint8_t* n = realloc(rgb, need); if (n) { rgb = n; rgb_cap = need; } }
      // THE DUMP READS THE FRAME THAT WAS PRESENTED, whichever path presented it — `fb`, at
      // `fbstride`. Dumping g_rgba here would have been the quiet disaster of this change: on the
      // CA path g_rgba is never written, so every gate that checks pixels would have been
      // checking a stale or zeroed buffer while the screen showed something else entirely. The
      // scalar fallback takes the stride too.
      if (rgb && rgb_cap >= need && !scalar_forced()) {
        const vImage_Buffer s4 = { fb,  (vImagePixelCount)H, (vImagePixelCount)W, fbstride };
        const vImage_Buffer d3 = { rgb, (vImagePixelCount)H, (vImagePixelCount)W, (size_t)W * 3 };
        if (vImageConvert_BGRA8888toRGB888(&s4, &d3,
                                           vimg_flags_px((size_t)W * (size_t)H)) == kvImageNoError)
          fwrite(rgb, 1, need, f);
        else
          dump_rows_scalar(f, fb, fbstride, W, H);
      } else {
        dump_rows_scalar(f, fb, fbstride, W, H);
      }
      fclose(f);
    }
  }
  prof_add(PH_DUMP, _td);
  prof_add(PH_FRAME, _tf);
  g_frames++;
  // The PPM dump is a fixed-size rolling window, so counting files saturates and hides the real
  // rate. Log the true count periodically so throughput is measurable rather than guessed.
  if (g_frames == 1 || g_frames % 10 == 0)
    fprintf(stderr, "[wl] presented frame %llu (%dx%d)\n", (unsigned long long)g_frames, W, H);
  if (prof_on()) {
    const char* short_run = getenv("WLSRV_PROF_SHORT");
    const uint64_t interval = short_run && *short_run == '1' ? 10 : 50;
    if (g_frames % interval == 0) prof_report();
  }
}

#ifndef WL_HEADLESS
// Compose a detached toplevel and only its popup chain into that window's own
// coordinate space. The legacy primary composite stays unchanged when the flag
// is off, and no popup is moved until its xdg parent identifies the owner.
static int present_secondary(struct surface* top) {
  if (!top || !top->rgba || top->rgba_w <= 0 || top->rgba_h <= 0) return -1;
  int W = top->rgba_w, H = top->rgba_h;
  size_t size = (size_t)W * (size_t)H * 4;
  static uint8_t* scratch;
  static size_t capacity;
  // The host presenter takes its own NSData copy before returning. When
  // popups have separate native windows, forwarding this retained surface
  // directly avoids an otherwise redundant full-frame scratch copy.
  const uint8_t* pixels = top->rgba;
  for (uint32_t pass = 1; pass < g_stack_next; pass++) {
    for (int i = 0; i < MAX_SURFACE; i++) {
      struct surface* popup = &g_surf[i];
      if (!popup->used || !popup->xdg_popup || !popup->mapped ||
          popup->popup_owner != top->id || popup->stack != pass || !popup->rgba) continue;
      // Popups have their own borderless NSWindows, allowing them to extend
      // beyond the toplevel content rect. Leave only a clipped fallback here
      // if the host popup window could not be opened.
      if (rd_secondary_popup_present(popup->id, top->id, popup->px, popup->py,
                                     popup->rgba, popup->rgba_w, popup->rgba_h) == 0) continue;
      if (pixels == top->rgba) {
        if (size > capacity) {
          uint8_t* next = realloc(scratch, size);
          if (!next) return -1;
          scratch = next; capacity = size;
        }
        memcpy(scratch, top->rgba, size);
        pixels = scratch;
      }
      for (int y = 0; y < popup->rgba_h; y++) {
        int dy = popup->py + y;
        if (dy < 0 || dy >= H) continue;
        const uint8_t* src = popup->rgba + (size_t)y * popup->rgba_w * 4;
        for (int x = 0; x < popup->rgba_w; x++, src += 4) {
          int dx = popup->px + x;
          if (dx < 0 || dx >= W) continue;
          uint8_t* dst = scratch + ((size_t)dy * W + dx) * 4;
          unsigned alpha = src[3];
          if (alpha == 255) { memcpy(dst, src, 4); }
          else if (alpha) {
            for (int c = 0; c < 3; c++)
              dst[c] = (uint8_t)((src[c] * alpha + dst[c] * (255 - alpha)) / 255);
            dst[3] = 255;
          }
        }
      }
    }
  }
  const char* dump = getenv("RDWL_MULTIWINDOW_DUMP");
  if (dump) {
    static unsigned frame;
    char path[512];
    snprintf(path, sizeof path, "%s%03u.ppm", dump, frame++ % 8);
    FILE* file = fopen(path, "wb");
    if (file) {
      fprintf(file, "P6\n%d %d\n255\n", W, H);
      for (size_t i = 0; i < (size_t)W * H; i++) {
        uint8_t rgb[3] = { pixels[i * 4 + 2], pixels[i * 4 + 1], pixels[i * 4] };
        fwrite(rgb, 1, 3, file);
      }
      fclose(file);
    }
  }
  return rd_secondary_present(top->id, pixels, (uint32_t)W, (uint32_t)H);
}
#endif


// ---------------------------------------------------------------------------
// input: host events -> wl_pointer / wl_keyboard
// ---------------------------------------------------------------------------
// Wayland coordinates on the wire are "fixed": signed 24.8. Everything else is plain integers.
static uint32_t wl_fixed(int32_t v) { return (uint32_t)(v * 256); }

// wl_pointer from version 5 requires a frame event to terminate each logical group of events.
// Sending the group without it leaves clients waiting for the rest of the group forever.
static void ptr_frame(void) {
  for (int i = 0; i < g_nptr; i++)
    if (g_ptrs[i].ver >= 5) { struct ev e; ev_begin(&e, g_ptrs[i].id, 5); ev_send(&e); }
}

// The focus surface must still EXIST. A destroyed wl_surface id is not merely stale: libwayland
// demarshals an object argument naming a dead id as NULL, and GTK4's pointer_handle_enter opens
// with `if (!surface) return;` — so the enter is dropped, seat->pointer_info.focus stays NULL, and
// pointer_handle_button/pointer_handle_motion then open with `if (!seat->pointer_info.focus)
// return;` too. Every click and every hover is silently discarded while the window keeps painting
// perfectly. That is the whole "renders but nothing is clickable" failure, so nothing may address
// input at a surface without first checking the id is live.
static int surface_alive(uint32_t sid) {
  if (!sid) return 0;
  struct object* o = obj_get(sid);
  return o && o->iface == IF_SURFACE;
}

static int focus_alive(void) { return surface_alive(g_focus_surface); }

// Enter every wl_pointer that has not been told about g_ptr_surface yet. (lx,ly) are already in
// THAT surface's coordinates. The liveness guard and the per-resource guard are orthogonal and
// both belong: the first decides whether this surface may be named at all, the second which
// resources still need to hear it.
static void ptr_enter_if_needed(int32_t lx, int32_t ly) {
  if (!surface_alive(g_ptr_surface)) return;
  int entered = 0, already = 0;
  for (int i = 0; i < g_nptr; i++) {
    if (g_ptrs[i].entered) { already++; continue; }
    struct ev e; ev_begin(&e, g_ptrs[i].id, 0);           // wl_pointer.enter
    ev_u32(&e, next_serial());
    ev_u32(&e, g_ptr_surface);
    ev_u32(&e, wl_fixed(lx)); ev_u32(&e, wl_fixed(ly));
    ev_send(&e);
    g_ptrs[i].entered = 1;
    if (g_ptrs[i].ver >= 5) { struct ev f; ev_begin(&f, g_ptrs[i].id, 5); ev_send(&f); }
    wlog("-> wl_pointer@%u.enter surface %u at (%d,%d)", g_ptrs[i].id, g_ptr_surface, lx, ly);
    entered++;
  }
  // The enter published the position — but ONLY to the resources that received one. Claiming it
  // for the whole seat is what made a click land 55x33 px away from where it happened: a client
  // holding two wl_pointers (GTK4 binds wl_seat twice and creates the second one while the
  // pointer is already inside the toplevel) would have the newcomer's enter set g_sent, and
  // ptr_goto()'s "nothing new to say" dedup then swallowed the motion the ALREADY-entered
  // resource was owed. It was told nothing at all, and wl_pointer.button carries no coordinates,
  // so the next press was delivered at the stale point. Only latch g_sent when every resource
  // was entered by THIS call; otherwise leave it, so the dedup falls through and the motion goes
  // to everyone. The resources just entered get one redundant motion at the same coordinates,
  // which is harmless and protocol-legal. See src/test_wlflap.c.
  if (entered && !already) { g_sent_x = lx; g_sent_y = ly; }
}

// Motion / button / axis all go to EVERY wl_pointer the client holds; see the note on g_ptrs.
// Per-resource ordering is what the protocol constrains, and each resource still sees its own
// events in order, so interleaving between resources is fine.
static void ptr_motion(int32_t x, int32_t y) {
  for (int i = 0; i < g_nptr; i++) {
    struct ev e; ev_begin(&e, g_ptrs[i].id, 2);           // wl_pointer.motion
    ev_u32(&e, now_ms());
    ev_u32(&e, wl_fixed(x)); ev_u32(&e, wl_fixed(y));
    ev_send(&e);
  }
}

static void ptr_button(uint32_t code, uint32_t state) {
  uint32_t s = next_serial(), t = now_ms();
  if (code == 0x110) {
    if (state) {
      struct surface* pointer = surface_of(g_ptr_surface);
      g_interactive_owner = pointer && pointer->xdg_popup ? pointer->popup_owner : g_ptr_surface;
      g_interactive_serial = s;
      g_interactive_pressed = 1;
    } else g_interactive_pressed = 0;
  }
  for (int i = 0; i < g_nptr; i++) {
    struct ev e; ev_begin(&e, g_ptrs[i].id, 3);           // wl_pointer.button
    ev_u32(&e, s);
    ev_u32(&e, t);
    ev_u32(&e, code);                                     // evdev BTN_LEFT 0x110 / RIGHT / MIDDLE
    ev_u32(&e, state);                                    // 1 pressed, 0 released
    ev_send(&e);
    wlog("-> wl_pointer@%u.button 0x%x state %u at (%d,%d)",
         g_ptrs[i].id, code, state, g_ptr_x, g_ptr_y);
  }
}

// One scroll group per resource: axis_source, then the discrete count, then the axis itself.
// v5 mandates the first two and GTK4 reads axis_source to classify the scroll.
static void ptr_axis(uint32_t axis, int32_t discrete, int32_t value) {
  uint32_t t = now_ms();
  for (int i = 0; i < g_nptr; i++) {
    if (g_ptrs[i].ver >= 5) {
      struct ev s; ev_begin(&s, g_ptrs[i].id, 6); ev_u32(&s, 0); ev_send(&s);   // 0 = wheel
      struct ev d; ev_begin(&d, g_ptrs[i].id, 8);
      ev_u32(&d, axis); ev_u32(&d, (uint32_t)discrete); ev_send(&d);
    }
    struct ev e; ev_begin(&e, g_ptrs[i].id, 4);           // wl_pointer.axis
    ev_u32(&e, t); ev_u32(&e, axis); ev_u32(&e, wl_fixed(value));
    ev_send(&e);
  }
}

// A keyboard focus-in carries the set of keys already held. We never synthesise held keys, so
// the array is empty — correct, and it keeps the client from thinking a key is stuck down.
static void kbd_enter_if_needed(void) {
  if (!focus_alive()) return;
  for (int i = 0; i < g_nkbd; i++) {
    if (g_kbds[i].entered) continue;
    struct ev e; ev_begin(&e, g_kbds[i].id, 1);           // wl_keyboard.enter
    ev_u32(&e, next_serial());
    ev_u32(&e, g_focus_surface);
    ev_array(&e, NULL, 0);
    ev_send(&e);
    g_kbds[i].entered = 1;
  }
}

static void kbd_modifiers(uint32_t mods) {
  uint32_t s = next_serial();
  for (int i = 0; i < g_nkbd; i++) {
    struct ev e; ev_begin(&e, g_kbds[i].id, 4);           // wl_keyboard.modifiers
    ev_u32(&e, s);
    ev_u32(&e, mods);                                     // depressed
    ev_u32(&e, 0);                                        // latched
    ev_u32(&e, (mods & GPUWIN_MOD_CAPS) ? GPUWIN_MOD_CAPS : 0);   // locked
    ev_u32(&e, 0);                                        // group
    ev_send(&e);
  }
}

static void kbd_key(uint32_t code, uint32_t state) {
  uint32_t s = next_serial(), t = now_ms();
  for (int i = 0; i < g_nkbd; i++) {
    struct ev e; ev_begin(&e, g_kbds[i].id, 3);           // wl_keyboard.key
    ev_u32(&e, s); ev_u32(&e, t);
    ev_u32(&e, code);                                     // evdev keycode, as the protocol wants
    ev_u32(&e, state);
    ev_send(&e);
  }
}

// Synthesise pointer motion by POLLING the cursor, and deduplicate against motion that arrived
// as an event.
//
// Hover must not depend on Cocoa delivering NSEventTypeMouseMoved: those only reach a local event
// monitor while the app is active AND the window is key, so hover dies exactly when a user
// clicks something else and comes back. gpuwin_cursor() is poll-based, documented thread-safe,
// and independent of the responder chain — it is what xeyes has always tracked the pointer with.
// So the event path is now an optimisation for latency, and this is the source of truth.
static int32_t g_poll_x = INT32_MIN, g_poll_y = INT32_MIN;

// Withdraw the pointer from whatever surface it is on. A leave NAMES that surface, so it is under
// the same liveness rule as an enter: if the surface is already gone the leave may not be sent at
// all and the entered state is dropped silently, because an event naming a dead id demarshals to
// NULL in the client and poisons its focus for the rest of the run.
static void ptr_leave(void) {
  int alive = surface_alive(g_ptr_surface);
  for (int i = 0; i < g_nptr; i++) {
    if (!g_ptrs[i].entered) continue;
    g_ptrs[i].entered = 0;
    if (!alive) continue;
    struct ev e; ev_begin(&e, g_ptrs[i].id, 1);               // wl_pointer.leave
    ev_u32(&e, next_serial()); ev_u32(&e, g_ptr_surface);
    ev_send(&e);
    if (g_ptrs[i].ver >= 5) { struct ev f; ev_begin(&f, g_ptrs[i].id, 5); ev_send(&f); }
  }
  g_ptr_surface = 0;
  g_sent_x = g_sent_y = INT32_MIN;
}

// Hit-test a WINDOW-local point. Mapped popups sit on top of the toplevel and are tested first,
// topmost (highest stack) winning; the answer is the surface the event belongs to PLUS the point
// in that surface's own coordinates, which is what wl_pointer.enter/motion carry. While one
// surface filled the window, window-global and surface-local were the same number and the
// distinction did not exist. With a menu on screen they differ by the menu's origin, and feeding
// a popup window-global coordinates puts every hover on the wrong row.
static uint32_t hit_surface(int x, int y, int32_t* lx, int32_t* ly) {
  struct surface* best = NULL;
  for (int i = 0; i < MAX_SURFACE; i++) {
    struct surface* s = &g_surf[i];
    if (!s->used || !s->xdg_popup || !s->mapped) continue;
    if (multiwindow_enabled() && toplevel_surface() && s->popup_owner &&
        s->popup_owner != toplevel_surface()->id) continue;
    if (x < s->px || y < s->py || x >= s->px + s->rgba_w || y >= s->py + s->rgba_h) continue;
    if (!best || s->stack > best->stack) best = s;
  }
  if (best) { *lx = x - best->px; *ly = y - best->py; return best->id; }

  // SECONDARY TOPLEVELS (dialogs), under the popups but over the primary — the same order
  // recomposite() draws them in, because a hit-test that disagrees with the compositing order
  // sends clicks to a surface the user cannot see.
  //
  // This loop was MISSING while recomposite() already published each dialog's origin with the
  // comment "publish it: hit-testing reads px/py". Nothing read it. The consequence is not a
  // dialog that ignores clicks — it is an application that ignores ALL of them: every press
  // inside the dialog was delivered to the PRIMARY toplevel underneath, at unoffset coordinates,
  // and a toolkit showing a MODAL dialog correctly refuses input to the window behind it. So the
  // whole app goes dead while painting perfectly.
  //
  // MEASURED on Qt Designer, which opens a modal "New Form" dialog at startup, so this was 100%
  // of the app: clicking its "Close" button at (647,561) logged
  // `wl_pointer@12.enter surface 14` — surface 14 being the PRIMARY (`get_toplevel 19 (surface
  // 14)`), while the dialog is surface 20. Not one control in the process was reachable.
  //
  // Array order, and the LAST match rather than the first, is deliberate: recomposite() blends
  // secondaries in ascending array order, so the last one drawn is the visible one on any overlap.
  // Popups keep their own explicit `stack` ordering above; toplevels have no stack, so agreeing
  // with the draw loop is the only ordering that cannot silently diverge from it.
  struct surface* primary = toplevel_surface();
  struct surface* dlg = NULL;
  for (int i = 0; i < MAX_SURFACE; i++) {
    struct surface* s = &g_surf[i];
    if (!s->used || !s->xdg_toplevel || s == primary || !s->mapped) continue;
    if (multiwindow_enabled()) continue;  // primary-window input must not hit a detached dialog
    if (!s->rgba || s->rgba_w <= 0 || s->rgba_h <= 0) continue;
    if (x < s->px || y < s->py || x >= s->px + s->rgba_w || y >= s->py + s->rgba_h) continue;
    dlg = s;
  }
  if (dlg) { *lx = x - dlg->px; *ly = y - dlg->py; return dlg->id; }

  // Outside the window the pointer is not hovering anything, and saying otherwise leaves a client
  // drawing hover highlights for a cursor that has left. Before the first frame there is no
  // rectangle to be outside OF, and the old event path delivered unconditionally in that window
  // (only the poller applied the test); keep delivering, or every event that arrives between
  // get_toplevel and the first present is dropped.
  int have_rect = (g_surf_w != 0 && g_surf_h != 0);
  int inside = !have_rect ||
               (x >= 0 && y >= 0 && (uint32_t)x < g_surf_w && (uint32_t)y < g_surf_h);
  if (!inside) return 0;
  *lx = x; *ly = y;
  struct surface* top = toplevel_surface();
  return top ? top->id : g_focus_surface;   // role-less client (bare EGL) keeps the old behaviour
}

// THE single place the pointer's position is published. Every path — poll, motion event, and
// crucially button — goes through it, so a click can never be delivered at a stale position or
// to the wrong surface.
static void ptr_goto(int x, int y) {
  g_ptr_host_window = 0;
  int moved = (x != g_ptr_x || y != g_ptr_y);
  g_ptr_x = x; g_ptr_y = y;
  // A hardware cursor is the compositor's to draw (wl_pointer.set_cursor). Hyprland does NOT
  // re-render its output when the pointer moves — it expects the compositor to move the cursor —
  // so a position change must re-present the frame even when no new buffer arrived.
  if (moved && g_cursor_surface) recomposite();
  if (!g_nptr) return;
  int32_t lx = 0, ly = 0;
  uint32_t tgt = hit_surface(x, y, &lx, &ly);
  if (!surface_alive(tgt)) tgt = 0;
  if (tgt != g_ptr_surface) {
    ptr_leave();                            // clears g_ptr_surface and every entered flag
    g_ptr_surface = tgt;
  }
  if (!g_ptr_surface) return;
  ptr_enter_if_needed(lx, ly);
  if (lx == g_sent_x && ly == g_sent_y) return;   // nothing new to say
  ptr_motion(lx, ly);
  ptr_frame();
  g_sent_x = lx; g_sent_y = ly;
}

// A detached host window reports coordinates local to its own Wayland toplevel.
// Do not pass them through the primary window's hit test or its content rect.
#ifndef WL_HEADLESS
static uint32_t hit_secondary_surface(uint32_t owner, int32_t x, int32_t y,
                                      int32_t* local_x, int32_t* local_y) {
  struct surface* best = NULL;
  for (int i = 0; i < MAX_SURFACE; i++) {
    struct surface* popup = &g_surf[i];
    if (!popup->used || !popup->xdg_popup || !popup->mapped ||
        popup->popup_owner != owner || !popup->rgba) continue;
    if (x < popup->px || y < popup->py ||
        x >= popup->px + popup->rgba_w || y >= popup->py + popup->rgba_h) continue;
    if (!best || popup->stack > best->stack) best = popup;
  }
  if (best) { *local_x = x - best->px; *local_y = y - best->py; return best->id; }
  *local_x = x; *local_y = y;
  return owner;
}

static void ptr_goto_secondary(uint32_t sid, int32_t x, int32_t y) {
  if (!surface_alive(sid)) return;
  g_ptr_host_window = sid;
  g_ptr_x = x; g_ptr_y = y;
  int32_t local_x = x, local_y = y;
  uint32_t target = hit_secondary_surface(sid, x, y, &local_x, &local_y);
  if (target != g_ptr_surface) {
    ptr_leave();
    g_ptr_surface = target;
  }
  ptr_enter_if_needed(local_x, local_y);
  if (local_x == g_sent_x && local_y == g_sent_y) return;
  ptr_motion(local_x, local_y);
  ptr_frame();
  g_sent_x = local_x; g_sent_y = local_y;
}
#endif

static void repaint_popup_owner(uint32_t owner_id) {
#ifndef WL_HEADLESS
  struct surface* owner = surface_of(owner_id);
  if (multiwindow_enabled() && owner && owner != toplevel_surface() && owner->mapped) {
    if (present_secondary(owner) == 0) {
      if (g_ptr_host_window == owner->id)
        ptr_goto_secondary(owner->id, g_ptr_x, g_ptr_y);
      return;
    }
  }
#endif
  recomposite();
  if (!multiwindow_enabled() || !g_ptr_host_window)
    ptr_goto(g_ptr_x, g_ptr_y);
}

// xdg-shell places a popup by anchoring a point on the anchor rect and then hanging the popup off
// that point according to the gravity. The two enums share values: 0 none, 1 top, 2 bottom,
// 3 left, 4 right, 5 top_left, 6 bottom_left, 7 top_right, 8 bottom_right.
static void popup_place(struct surface* s, const struct positioner* p) {
  int32_t x = p->ax, y = p->ay;
  switch (p->anchor) {                                           // a point ON the anchor rect
  case 1: x += p->aw / 2;                  break;                // top
  case 2: x += p->aw / 2; y += p->ah;      break;                // bottom
  case 3:                 y += p->ah / 2;  break;                // left
  case 4: x += p->aw;     y += p->ah / 2;  break;                // right
  case 5:                                  break;                // top_left
  case 6:                 y += p->ah;      break;                // bottom_left
  case 7: x += p->aw;                      break;                // top_right
  case 8: x += p->aw;     y += p->ah;      break;                // bottom_right
  default: x += p->aw / 2; y += p->ah / 2; break;                // none = centre
  }
  switch (p->gravity) {                                          // which way the popup hangs
  case 1: x -= s->pw / 2; y -= s->ph;      break;                // top
  case 2: x -= s->pw / 2;                  break;                // bottom
  case 3: x -= s->pw;     y -= s->ph / 2;  break;                // left
  case 4:                 y -= s->ph / 2;  break;                // right
  case 5: x -= s->pw;     y -= s->ph;      break;                // top_left
  case 6: x -= s->pw;                      break;                // bottom_left
  case 7:                 y -= s->ph;      break;                // top_right
  case 8:                                  break;                // bottom_right
  default: x -= s->pw / 2; y -= s->ph / 2; break;                // none = centre
  }
  s->rx = x + p->ox; s->ry = y + p->oy;
}

// xdg_popup.popup_done — "you have been dismissed". The client is expected to destroy the popup in
// response, and that is what makes a menu close when you click elsewhere. A real compositor drives
// this from the seat grab handed over by xdg_popup.grab; we do not take a real grab, and this is
// the part clients actually depend on.
static void popup_dismiss_if_outside(int x, int y) {
  struct surface* best = NULL;
  struct surface* primary = toplevel_surface();
  for (int i = 0; i < MAX_SURFACE; i++) {
    struct surface* s = &g_surf[i];
    if (!s->used || !s->xdg_popup || !s->mapped) continue;
    if (multiwindow_enabled() && primary && s->popup_owner && s->popup_owner != primary->id)
      continue;
    if (x >= s->px && y >= s->py && x < s->px + s->rgba_w && y < s->py + s->rgba_h) return;
    if (!best || s->stack > best->stack) best = s;
  }
  if (!best) return;
  struct ev e; ev_begin(&e, best->xdg_popup, 1); ev_send(&e);   // xdg_popup.popup_done
  wlog("popup_done -> %u (press at %d,%d is outside every popup)", best->xdg_popup, x, y);
}

#ifndef WL_HEADLESS
static void popup_dismiss_if_outside_secondary(uint32_t owner, int32_t x, int32_t y) {
  struct surface* best = NULL;
  for (int i = 0; i < MAX_SURFACE; i++) {
    struct surface* popup = &g_surf[i];
    if (!popup->used || !popup->xdg_popup || !popup->mapped || popup->popup_owner != owner) continue;
    if (x >= popup->px && y >= popup->py &&
        x < popup->px + popup->rgba_w && y < popup->py + popup->rgba_h) return;
    if (!best || popup->stack > best->stack) best = popup;
  }
  if (!best) return;
  struct ev e; ev_begin(&e, best->xdg_popup, 1); ev_send(&e);
  wlog("popup_done -> %u (outside detached toplevel %u)", best->xdg_popup, owner);
}
#endif

static void kbd_leave(void) {
  for (int i = 0; i < g_nkbd; i++) {
    if (!g_kbds[i].entered) continue;
    struct ev e; ev_begin(&e, g_kbds[i].id, 2);               // wl_keyboard.leave
    ev_u32(&e, next_serial()); ev_u32(&e, g_focus_surface);
    ev_send(&e);
    g_kbds[i].entered = 0;
  }
}

// Move KEYBOARD focus to `sid`, withdrawing from whatever held it. The pointer is not touched:
// it follows the cursor now, not the focus, and ptr_goto() re-hit-tests on the next tick anyway.
// The withdrawal must be a real wl_keyboard.leave, because a client left believing it still has
// focus will not take it again.
static void set_focus_surface(uint32_t sid) {
  struct surface* target = surface_of(sid);
  if (target && target->xdg_toplevel) g_toplevel_surface = sid;
  if (g_focus_surface == sid) return;
  kbd_leave();
  g_focus_surface = sid;
  g_poll_x = INT32_MIN; g_poll_y = INT32_MIN;   // force the poller to re-announce, enter included
  wlog("keyboard focus -> surface %u", sid);
}

// The focused surface has been DESTROYED. No leave may be sent: its id is dead, and an event
// naming a dead id demarshals to NULL in the client. Drop the entered state silently and fall
// back to the toplevel, which is the only surface guaranteed to outlive the popups.
static void focus_surface_destroyed(uint32_t sid) {
  if (sid == g_toplevel_surface) g_toplevel_surface = 0;
  if (sid != g_focus_surface) return;
  seatres_clear_entered(g_kbds, g_nkbd);
  g_focus_surface = g_toplevel_surface;
  g_poll_x = INT32_MIN; g_poll_y = INT32_MIN;
  wlog("focus surface %u destroyed -> falling back to %u", sid, g_focus_surface);
}

static void pump_pointer_poll(void) {
  if (!g_nptr) return;
#ifndef WL_HEADLESS
  if (multiwindow_enabled() && rd_secondary_key_window_id()) return;
#endif
  int x = 0, y = 0;
  gpuwin_cursor(&x, &y);
  if (x == g_poll_x && y == g_poll_y) return;                 // unchanged: nothing to report
  g_poll_x = x; g_poll_y = y;
  // ptr_goto() hit-tests, which covers "outside everything" by targeting 0 and sending the leave.
  ptr_goto(x, y);
}

// Drain everything the window layer has queued and translate it. Called from the compositor
// thread on every poll tick, so input latency is bounded by the poll timeout.
// A frame terminates ONE LOGICAL GROUP, not one poll tick. Sending a single frame after draining
// the whole host queue merges a motion, a press and a release into one group; a client is then
// entitled to treat them as one simultaneous state change, and GTK4 in particular keeps exactly
// one pending event (gdk_wayland_seat_set_frame_event stores a single frame.event pointer), so
// events sharing a group are liable to be overwritten rather than delivered. Each host event
// therefore gets its own frame.
static void pump_input(void) {
  struct gpuwin_event g;
  int saw_motion = 0;
  while (gpuwin_next_event(&g)) {
    if (g.mods != g_mods) { g_mods = g.mods; kbd_enter_if_needed(); kbd_modifiers(g_mods); }
    switch (g.type) {
    case GPUWIN_EV_MOTION:
      saw_motion = 1;
      g_poll_x = g.x; g_poll_y = g.y;          // keep the poller in step, so it does not re-send
      ptr_goto(g.x, g.y);
      break;
    case GPUWIN_EV_BUTTON:
      // Position FIRST, through the one shared path. wl_pointer.button carries no coordinates, so
      // the client acts on the last position it was told — and the Cocoa event path does not run
      // while the app is inactive, so the first click after a re-focus can be the only event that
      // carries the new coordinates. ptr_goto also decides WHICH surface the press belongs to,
      // which is what routes a click into an open menu rather than the window behind it.
      g_poll_x = g.x; g_poll_y = g.y;
      if (multiwindow_enabled()) {
        struct surface* primary = toplevel_surface();
        if (primary) set_focus_surface(primary->id);
      }
      ptr_goto(g.x, g.y);
      if (!g_nptr || !g_ptr_surface) break;
      // A press outside every popup dismisses the topmost one. Without popup_done a GTK menu
      // never closes and the application is stuck behind it.
      if (g.state) popup_dismiss_if_outside(g.x, g.y);
      ptr_button(g.code, g.state);
      ptr_frame();
      break;
    case GPUWIN_EV_SCROLL:
      if (!g_nptr || !g_ptr_surface) break;
      // From v5 an axis group must name its source, and a wheel must also report the discrete
      // click count. GTK4 reads axis_source to decide whether the scroll is kinetic (it stops
      // one on wl_pointer.axis_stop) or a wheel (it stops on the discrete count), so an axis
      // with neither is a scroll the toolkit cannot classify.
      if (g.y) ptr_axis(0, g.y, g.y * 10);                // vertical
      if (g.x) ptr_axis(1, g.x, g.x * 10);                // horizontal
      ptr_frame();
      break;
    case GPUWIN_EV_KEY:
      if (multiwindow_enabled()) {
        struct surface* primary = toplevel_surface();
        if (primary) set_focus_surface(primary->id);
      }
      if (g.state && g.code == 47 && (g.mods & GPUWIN_MOD_CTRL) &&
          gpuwin_clipboard_count() != atomic_load(&g_guest_pasteboard_count))
        import_selection_from_mac();
      kbd_enter_if_needed();
      if (!g_nkbd) break;
      kbd_key(g.code, g.state);
      break;
    case GPUWIN_EV_RESIZE: {
      struct surface* s = toplevel_surface();
      if (!s || !s->xdg_toplevel || !s->xdg_surface) break;
      struct ev t; ev_begin(&t, s->xdg_toplevel, 0);
      ev_u32(&t, (uint32_t)g.x); ev_u32(&t, (uint32_t)g.y);
      uint32_t active = 4; ev_array(&t, &active, sizeof active); ev_send(&t);
      struct ev c; ev_begin(&c, s->xdg_surface, 0);
      ev_u32(&c, next_serial()); ev_send(&c);
      break;
    }
    default:
      break;
    }
  }
#ifndef WL_HEADLESS
  if (multiwindow_enabled()) {
    uint32_t wid = 0;
    while (rd_secondary_next_event(&wid, &g)) {
      struct surface* s = surface_of(wid);
      if (!s || !s->used || !s->mapped || !s->xdg_toplevel || s == toplevel_surface()) continue;
      if (g.mods != g_mods) { g_mods = g.mods; kbd_enter_if_needed(); kbd_modifiers(g_mods); }
      switch (g.type) {
      case GPUWIN_EV_MOTION:
        saw_motion = 1;
        ptr_goto_secondary(s->id, g.x, g.y);
        break;
      case GPUWIN_EV_BUTTON:
        saw_motion = 1;
        set_focus_surface(s->id);
        ptr_goto_secondary(s->id, g.x, g.y);
        if (g.state) popup_dismiss_if_outside_secondary(s->id, g.x, g.y);
        if (g_nptr && g_ptr_surface) { ptr_button(g.code, g.state); ptr_frame(); }
        break;
      case GPUWIN_EV_KEY:
        set_focus_surface(s->id);
        kbd_enter_if_needed();
        if (g_nkbd) kbd_key(g.code, g.state);
        break;
      case GPUWIN_EV_SCROLL:
        if (g.y) ptr_axis(0, g.y, g.y * 10);
        if (g.x) ptr_axis(1, g.x, g.x * 10);
        ptr_frame();
        break;
      case GPUWIN_EV_RESIZE: {
        if (!s->xdg_surface || g.x <= 0 || g.y <= 0) break;
        struct ev configure; ev_begin(&configure, s->xdg_toplevel, 0);
        ev_u32(&configure, (uint32_t)g.x); ev_u32(&configure, (uint32_t)g.y);
        uint32_t active = 4; ev_array(&configure, &active, sizeof active); ev_send(&configure);
        struct ev done; ev_begin(&done, s->xdg_surface, 0);
        ev_u32(&done, next_serial()); ev_send(&done);
        wlog("detached toplevel %u configured %dx%d", s->xdg_toplevel, g.x, g.y);
        break;
      }
      case 9: { // RD_SECONDARY_EV_CLOSE: ask guest to destroy this xdg_toplevel
        struct ev close_event; ev_begin(&close_event, s->xdg_toplevel, 1);
        ev_send(&close_event);
        wlog("native close requested for detached toplevel %u", s->xdg_toplevel);
        break;
      }
      default:
        break;
      }
    }
  }
#endif
  if (!saw_motion) pump_pointer_poll();        // no event this tick: fall back to polling
}

// ---------------------------------------------------------------------------
// request dispatch
// ---------------------------------------------------------------------------
static void send_frame_done(struct surface* s) {
  for (int i = 0; i < s->nframe; i++) {
    struct ev e; ev_begin(&e, s->frame_cb[i], 0); ev_u32(&e, now_ms()); ev_send(&e);
    delete_id(s->frame_cb[i]);
    obj_del(s->frame_cb[i]);
  }
  s->nframe = 0;
  s->direct_frame_due_ms = 0;
  s->direct_frame_target = 0;
  s->direct_frame_output = 0;
}

static void pump_present_feedback(void) {
  for(int i=0;i<MAX_SURFACE;i++) {
    struct surface* s=&g_surf[i];
    if(s->used&&s->direct_frame_target&&
       rd_feedback_completed(s->direct_frame_output)>=s->direct_frame_target)
      send_frame_done(s);
  }
}

static void pump_direct_frame_callbacks(void) {
  uint32_t now=now_ms();
  for (int i=0;i<MAX_SURFACE;i++) {
    struct surface* s=&g_surf[i];
    if (s->used && s->direct_frame_due_ms &&
        (int32_t)(now-s->direct_frame_due_ms)>=0) send_frame_done(s);
  }
}

// Is this the frame mode 2 can take straight from the shm pool to the screen? Only when the
// primary toplevel is the ONLY thing on screen — the moment a popup or a dialog is mapped, the
// frame has to be assembled from several surfaces and each one needs its own retained copy back.
// This is deliberately conservative: charter scope is one surface, and a wrong answer here draws
// a menu-less window, which looks like working software.
static int ca_direct_ok(struct surface* self) {
  if (capres_mode() < 2 || !capres_enabled()) return 0;
  if (self != toplevel_surface() || !self->xdg_toplevel) return 0;
  for (int i = 0; i < MAX_SURFACE; i++) {
    struct surface* o = &g_surf[i];
    if (o == self || !o->used || !o->mapped) continue;
    if (o->xdg_toplevel || o->xdg_popup) return 0;      // something else is on screen
  }
  return 1;
}

static void surface_commit(struct surface* s) {
  if (s->has_pending_buffer) {
    s->current_buffer = s->pending_buffer;
    s->has_pending_buffer = 0;
  }
  if (s->current_buffer) {
    struct object* o = obj_get(s->current_buffer);
    // Only a real WINDOW goes to the screen. A client commits other surfaces too — most obviously
    // the cursor, a 24x24 image — and presenting those made the window flicker down to cursor
    // size whenever the pointer moved. Everything else still gets its buffer released and its
    // frame callback fired, or the client stalls waiting for them.
    // A POPUP also has an xdg_surface, so "has a shell role" is not the test: presenting a menu's
    // buffer would resize the Metal window down to the menu and put the menu on screen INSTEAD OF
    // the application. Only a toplevel — or the role-less surface that already holds focus — is
    // the window.
    int is_window = (s->xdg_toplevel != 0) ||
                    (s->xdg_surface == 0 && !s->is_popup && s->id == g_focus_surface);
    if (o && o->iface == IF_BUFFER && o->data < MAX_BUF && g_buf[o->data].used) {
#ifndef WL_HEADLESS
      // The named WAYLAND-2 IOSurface already reached its own Metal drawable.
      // The wl_shm buffer is only a release/frame-callback token: avoid the
      // otherwise redundant ~20 MB pixel conversion and CPU composite.
      if (is_window && s->xdg_toplevel && multiwindow_enabled() &&
          getenv("RDNATIVE_DIRECT_GPU") &&
          !strcmp(s->title, "aquamarine - WAYLAND-2")) {
        int has_popup = 0;
        for (int i = 0; i < MAX_SURFACE; i++)
          if (g_surf[i].used && g_surf[i].xdg_popup && g_surf[i].mapped &&
              g_surf[i].popup_owner == s->id) { has_popup = 1; break; }
        if (rd_output2_direct_allowed(has_popup) && rd_secondary_direct_claim(s->id)) {
          uint32_t width = g_buf[o->data].width, height = g_buf[o->data].height;
          if ((s->rgba_w != (int)width || s->rgba_h != (int)height) &&
              rd_secondary_open(s->id, width, height, s->title, 0) != 0)
            goto secondary_cpu_fallback;
          s->rgba_w = width;
          s->rgba_h = height;
          s->mapped = 1;
          struct ev e; ev_begin(&e, s->current_buffer, 0); ev_send(&e);
          s->current_buffer = 0;
          if (getenv("RDNATIVE_PRESENT_FEEDBACK") && s->nframe &&
              rd_feedback_submitted(1)) {
            s->direct_frame_output=1;
            s->direct_frame_target=rd_feedback_submitted(1);
          } else if (getenv("RDNATIVE_DIRECT_PACE") && s->nframe) {
            s->direct_frame_due_ms = now_ms() + 16;
            if (!s->direct_frame_due_ms) s->direct_frame_due_ms = 1;
          } else send_frame_done(s);
          static unsigned bypassed;
          if (++bypassed <= 2 || bypassed % 100 == 0)
            fprintf(stderr,"[gpu-output2] SHM pixels bypassed frame=%u; callback paced\n",bypassed);
          return;
        }
      }
secondary_cpu_fallback: ;
      // The opt-in ANGLE/IOSurface path has already handed this compositor
      // frame to the host Metal presenter. Keep Wayland's release and frame
      // callback contract, but do not read stale SHM bytes or present them over
      // the GPU frame. Other clients/surfaces keep the ordinary path.
      extern int gpuwin_direct_active(void);
      if (is_window && s->xdg_toplevel && s == toplevel_surface() &&
          getenv("RDNATIVE_DIRECT_GPU") && gpuwin_direct_active()) {
        static int said;
        s->rgba_w = g_buf[o->data].width;
        s->rgba_h = g_buf[o->data].height;
        s->mapped = 1;
        if (!said++) fprintf(stderr,"[wl] direct GPU output active; SHM commit is scheduling only\n");
        struct ev e; ev_begin(&e, s->current_buffer, 0); ev_send(&e);
        s->current_buffer = 0;
        if (getenv("RDNATIVE_PRESENT_FEEDBACK") && s->nframe && rd_feedback_submitted(0)) {
          s->direct_frame_output=0;
          s->direct_frame_target=rd_feedback_submitted(0);
        } else if (getenv("RDNATIVE_DIRECT_PACE") && s->nframe) {
          s->direct_frame_due_ms = now_ms() + 16;
          if (!s->direct_frame_due_ms) s->direct_frame_due_ms = 1;
        }
        else send_frame_done(s);
        return;
      }
#endif
      if (is_window) {
        // MODE 2's decision, made HERE because it decides whether convert_buffer() runs at all:
        // its destination is the IOSurface, which recomposite() is the one that acquires.
        // A dma-buf buffer has no shm pool, so it can never take this (or the convert) path.
        int direct = ca_direct_ok(s) && !g_buf[o->data].dmabuf;
        if (direct) {
          // recomposite() reads rgba_w/rgba_h to size the window and the surface, and on the
          // direct path convert_buffer_to() is what would normally set them — after the fact.
          s->rgba_w = g_buf[o->data].width;
          s->rgba_h = g_buf[o->data].height;
          g_ca_direct = &g_buf[o->data];
        }
        if (direct || convert_buffer(s, &g_buf[o->data])) {
          // Mark toplevels mapped on their first buffer, the same way popups are. recomposite()
          // needs it to know a secondary toplevel (a dialog) has content worth blending; without
          // it the dialog exists, takes input, and is never drawn.
          if (!s->mapped && s->xdg_toplevel) {
            s->mapped = 1;
            if (s != toplevel_surface())
              wlog("secondary toplevel %u mapped %dx%d (dialog, centred over the primary)",
                   s->xdg_toplevel, s->rgba_w, s->rgba_h);
          }
          int detached = 0;
#ifndef WL_HEADLESS
          if (multiwindow_enabled() && s->xdg_toplevel && s != toplevel_surface()) {
            uint32_t parent_id = s->parent_surface;
            if (parent_id && toplevel_surface() && parent_id == toplevel_surface()->id)
              parent_id = UINT32_MAX;        // host layer's existing primary NSWindow
            if (rd_secondary_open(s->id, (uint32_t)s->rgba_w, (uint32_t)s->rgba_h,
                                  s->title[0] ? s->title : "Linux dialog", parent_id) == 0 &&
                present_secondary(s) == 0) {
              detached = 1;
              wlog("detached toplevel surface %u -> separate NSWindow", s->id);
            } else {
              // If the host path fails, close every detached window and restore
              // the legacy single-window compositor for this guest process.
              g_multiwindow_failed = 1;
              for (int j = 0; j < MAX_SURFACE; j++)
                if (g_surf[j].xdg_toplevel && &g_surf[j] != toplevel_surface())
                  rd_secondary_close(g_surf[j].id);
              fprintf(stderr, "[wl] detached window failed; restored composite path\n");
            }
          }
#endif
          if (!detached) recomposite();
        }
        g_ca_direct = NULL;
      } else if (s->is_popup && s->xdg_popup) {
        // A popup IS drawn — over the toplevel, never instead of it. Excluding popups from
        // is_window stops them resizing the host window; it does not draw them, and until now
        // nothing did, so a GTK4 menu opened fully functional and completely invisible.
        if (convert_buffer(s, &g_buf[o->data])) {
          if (!s->mapped) {
            s->mapped = 1;
            wlog("popup %u mapped at %d,%d %dx%d",
                 s->xdg_popup, s->px, s->py, s->rgba_w, s->rgba_h);
          }
          int on_detached = 0;
#ifndef WL_HEADLESS
          struct surface* owner = surface_of(s->popup_owner);
          if (multiwindow_enabled() && owner && owner != toplevel_surface() && owner->mapped) {
            on_detached = present_secondary(owner) == 0;
            if (on_detached) {
              wlog("popup %u presented in detached toplevel %u", s->xdg_popup, owner->id);
              if (g_ptr_surface == owner->id || g_ptr_surface == s->id)
                ptr_goto_secondary(owner->id, g_ptr_x, g_ptr_y);
            }
          }
#endif
          if (!on_detached) {
            recomposite();
            ptr_goto(g_ptr_x, g_ptr_y);   // the menu may now be under the cursor
          }
        }
      } else if (!s->xdg_surface && !s->xdg_toplevel && !s->xdg_popup) {
        // A role-less surface — the hardware cursor (wl_pointer.set_cursor) AND a wl_subsurface
        // (KWin's desktop layers have no xdg role). Hyprland commits the cursor image here BEFORE
        // it names the surface via wl_pointer.set_cursor, so this cannot test g_cursor_surface.
        // Convert every role-less commit into the surface's own retained RGBA copy (NOT "present
        // it": that would resize the window down to the cursor, the exact flicker this path has
        // always avoided). recomposite() blends it over the frame once set_cursor has named it, or
        // over its parent when it is a subsurface.
        if (convert_buffer(s, &g_buf[o->data])) {
          if (s->is_subsurface && !s->mapped) s->mapped = 1;
          recomposite();
        }
      }
      // Release as soon as the pixels are consumed: Mesa's swrast recycles a small pool of
      // buffers and BLOCKS waiting for wl_buffer.release, so forgetting this is a hard hang.
      struct ev e; ev_begin(&e, s->current_buffer, 0); ev_send(&e);
    } else if (o && o->iface == IF_SINGLE_PIXEL_BUFFER) {
      // wp_single_pixel_buffer_v1: KWin's nested output attaches ONE colour to the main toplevel
      // as its background. The buffer has no size of its own, so fill_single_pixel() sizes the
      // fill from wp_viewport.set_destination (1024x768). The fill IS the surface's retained copy,
      // so recomposite() presents it through the ordinary Metal path.
      fill_single_pixel(s, o->data);
      if (!s->mapped && s->xdg_toplevel) s->mapped = 1;
      recomposite();
      struct ev e; ev_begin(&e, s->current_buffer, 0); ev_send(&e);
    }
    s->current_buffer = 0;
  } else if (s->xdg_surface && !s->acked) {
    // The role's first commit is deliberately buffer-less: answer it with a configure so the
    // client knows it may now attach one.
    struct object* t = obj_get(s->xdg_toplevel);
    if (t) {
      struct ev e; ev_begin(&e, s->xdg_toplevel, 0);        // xdg_toplevel.configure
      uint32_t fit_w = 0, fit_h = 0;
      const char* fit_name = !strcmp(s->title, "aquamarine - WAYLAND-1") ?
          "RDNATIVE_FIT_OUTPUT_1" :
          !strcmp(s->title, "aquamarine - WAYLAND-2") ? "RDNATIVE_FIT_OUTPUT_2" : NULL;
      const char* fit = fit_name ? getenv(fit_name) : NULL;
      unsigned parsed_w = 0, parsed_h = 0;
      char tail = 0;
      if (fit && sscanf(fit, "%ux%u%c", &parsed_w, &parsed_h, &tail) == 2 &&
          parsed_w >= 640 && parsed_h >= 480 && parsed_w <= 8192 && parsed_h <= 8192) {
        fit_w = parsed_w; fit_h = parsed_h;
      }
      ev_u32(&e, fit_w); ev_u32(&e, fit_h);  // 0x0 remains default for ordinary apps
      // ACTIVATED (xdg_toplevel.state 4). With no states at all GTK paints itself as a BACKGROUND
      // window and declines keyboard focus, which is indistinguishable from dead input.
      uint32_t states[1] = { 4 };
      ev_array(&e, states, sizeof states);
      ev_send(&e);
    }
    // A popup's xdg_surface.configure must be PRECEDED by xdg_popup.configure carrying the rect
    // the compositor chose; they are one configure sequence and the client acks the pair. (An
    // unpaired one is also sent at get_popup, because a toolkit that never hears any answer stops
    // responding rather than merely missing a menu — an extra configure is benign, a missing one
    // is fatal.)
    if (s->xdg_popup) {
      struct ev e; ev_begin(&e, s->xdg_popup, 0);            // xdg_popup.configure(x,y,w,h)
      ev_u32(&e, (uint32_t)s->rx); ev_u32(&e, (uint32_t)s->ry);
      ev_u32(&e, (uint32_t)s->pw); ev_u32(&e, (uint32_t)s->ph);
      ev_send(&e);
    }
    struct ev e; ev_begin(&e, s->xdg_surface, 0);            // xdg_surface.configure
    ev_u32(&e, next_serial());
    ev_send(&e);
  }
  // The direct Metal frame for a detached compositor output has already been
  // presented. Pace its Wayland callback just like the primary direct path;
  // otherwise the second output spins hundreds of full GPU frames per second.
#ifndef WL_HEADLESS
  if (getenv("RDNATIVE_DIRECT_PACE") && s->xdg_toplevel &&
      rd_secondary_direct_consumed(s->id) && s->nframe) {
    s->direct_frame_due_ms = now_ms() + 16;
    if (!s->direct_frame_due_ms) s->direct_frame_due_ms = 1;
    return;
  }
#endif
  send_frame_done(s);
}

static int take_fd(void) {
  if (g_nrfd <= 0) return -1;
  int fd = g_rfd[0];
  memmove(g_rfd, g_rfd + 1, (size_t)(--g_nrfd) * sizeof(int));
  return fd;
}

// Read a string argument at `a`; returns its length field and advances *off.
static const char* arg_str(const uint8_t* a, size_t len, size_t* off) {
  if (*off + 4 > len) return NULL;
  uint32_t l = rd32(a + *off); *off += 4;
  if (l == 0 || *off + l > len) return NULL;
  const char* s = (const char*)(a + *off);
  *off += (l + 3) & ~3u;
  return s;
}

static struct data_source* source_by_id(uint32_t id) {
  if (!id) return NULL;
  for (int i = 0; i < MAX_DATA_SOURCE; i++) if (g_sources[i].id == id) return &g_sources[i];
  return NULL;
}

static void send_selection_to(uint32_t device) {
  struct data_source* src = source_by_id(g_selection_source);
  uint32_t offer = 0;
  if (src) {
    for (int i = 0; i < 64; i++) {
      uint32_t candidate = SERVER_ID_LO + ((g_offer_next++ - SERVER_ID_LO) % 64);
      if (!obj_get(candidate)) { offer = candidate; break; }
    }
    if (offer) {
      obj_set(offer, IF_DATA_OFFER, 3, g_selection_source);
      struct ev d; ev_begin(&d, device, 0); ev_u32(&d, offer); ev_send(&d);
      for (int i = 0; i < src->nmime; i++) {
        struct ev m; ev_begin(&m, offer, 0); ev_str(&m, src->mime[i]); ev_send(&m);
      }
    }
  }
  struct ev s; ev_begin(&s, device, 5); ev_u32(&s, offer); ev_send(&s);
}

struct clipboard_capture { int fd; char mime[128]; };
static void* capture_clipboard(void* arg) {
  struct clipboard_capture* job = arg;
  char* data = malloc(1u << 20);
  size_t n = 0;
  if (data) for (;;) {
    ssize_t r = read(job->fd, data + n, (1u << 20) - n);
    if (r <= 0) break;
    n += (size_t)r;
    if (n == (1u << 20)) break;
  }
  close(job->fd);
  if (data && n) {
    gpuwin_clipboard_write(job->mime, data, n);
    atomic_store(&g_guest_pasteboard_count, gpuwin_clipboard_count());
  }
  free(data); free(job);
  return NULL;
}

static void export_selection_to_mac(void) {
  struct data_source* src = source_by_id(g_selection_source);
  if (!src) return;
  const char* preferred[] = { "x-special/gnome-copied-files", "text/uri-list",
                              "text/plain;charset=utf-8", "text/plain", NULL };
  const char* mime = NULL;
  for (int p = 0; preferred[p] && !mime; p++)
    for (int i = 0; i < src->nmime; i++)
      if (!strcmp(src->mime[i], preferred[p])) { mime = src->mime[i]; break; }
  if (!mime) return;
  int fds[2]; if (pipe(fds) < 0) return;
  struct clipboard_capture* job = calloc(1, sizeof *job);
  if (!job) { close(fds[0]); close(fds[1]); return; }
  job->fd = fds[0]; snprintf(job->mime, sizeof job->mime, "%s", mime);
  pthread_t t;
  if (pthread_create(&t, NULL, capture_clipboard, job) != 0) {
    free(job); close(fds[0]); close(fds[1]); return;
  }
  pthread_detach(t);
  struct ev e; ev_begin(&e, src->id, 1); ev_str(&e, mime); ev_send_fd(&e, fds[1]);
  close(fds[1]);
}

static void import_selection_from_mac(void) {
  g_selection_source = 0;
  for (int i = 0; i < g_ndevices; i++) {
    uint32_t offer = 0;
    for (int j = 0; j < 64; j++) {
      uint32_t candidate = SERVER_ID_LO + ((g_offer_next++ - SERVER_ID_LO) % 64);
      if (!obj_get(candidate)) { offer = candidate; break; }
    }
    if (!offer) continue;
    obj_set(offer, IF_DATA_OFFER, 3, 0); // source 0 means NSPasteboard
    struct ev d; ev_begin(&d, g_data_devices[i], 0); ev_u32(&d, offer); ev_send(&d);
    const char* mime[] = { "x-special/gnome-copied-files", "text/uri-list",
                           "text/plain;charset=utf-8", "text/plain" };
    for (int j = 0; j < 4; j++) {
      struct ev m; ev_begin(&m, offer, 0); ev_str(&m, mime[j]); ev_send(&m);
    }
    struct ev s; ev_begin(&s, g_data_devices[i], 5); ev_u32(&s, offer); ev_send(&s);
  }
}

static const char* iface_name(uint8_t i) {
  static const char* n[] = { "none","display","registry","callback","compositor","shm","shm_pool",
    "buffer","surface","region","seat","pointer","keyboard","touch","output","wm_base",
    "xdg_surface","xdg_toplevel","xdg_positioner","xdg_popup","subcompositor","subsurface","ddm",
    "dmabuf","dmabuf_params","dmabuf_feedback",
    "server_decoration_manager","server_decoration","data_source","data_device","data_offer",
    "single_pixel_manager","single_pixel_buffer","viewporter","viewport",
    "pointer_constraints","locked_pointer","confined_pointer",
    "presentation","presentation_feedback" };
  return i < sizeof n / sizeof n[0] ? n[i] : "?";
}

static void handle(uint32_t id, uint16_t op, const uint8_t* a, size_t len) {
  struct object* o = obj_get(id);
  if (!o) { wwarn("request for unknown object %u (op %u)", id, op); return; }
  wlog("<- %s@%u.%u (%zu bytes)", iface_name(o->iface), id, op, len);

  switch (o->iface) {

  // ---------------------------------------------------------------- wl_display
  case IF_DISPLAY:
    if (op == 0) {                                          // sync(new_id callback)
      uint32_t cb = len >= 4 ? rd32(a) : 0;
      wlog("sync -> callback %u", cb);
      struct ev e; ev_begin(&e, cb, 0); ev_u32(&e, next_serial()); ev_send(&e);
      delete_id(cb);
      return;
    }
    if (op == 1) {                                          // get_registry(new_id)
      uint32_t r = len >= 4 ? rd32(a) : 0;
      obj_set(r, IF_REGISTRY, 1, 0);
      for (int i = 0; i < NGLOBALS; i++) {
        struct ev e; ev_begin(&e, r, 0);                     // wl_registry.global
        ev_u32(&e, g_globals[i].name);
        ev_str(&e, g_globals[i].iface);
        ev_u32(&e, g_globals[i].version);
        ev_send(&e);
      }
      wlog("get_registry %u -> %d globals", r, NGLOBALS);
      return;
    }
    return;

  // --------------------------------------------------------------- wl_registry
  case IF_REGISTRY:
    if (op == 0) {                                          // bind(name, iface, version, new_id)
      size_t off = 0;
      if (len < 4) return;
      uint32_t name = rd32(a); off = 4;
      const char* iface = arg_str(a, len, &off);
      if (!iface || off + 8 > len) { post_error(id, 0, "malformed bind"); return; }
      uint32_t ver = rd32(a + off); off += 4;
      uint32_t nid = rd32(a + off);
      for (int i = 0; i < NGLOBALS; i++) {
        if (g_globals[i].name != name) continue;
        if (strcmp(g_globals[i].iface, iface) != 0) { post_error(id, 0, "bind name/iface mismatch"); return; }
        uint32_t use = ver < g_globals[i].version ? ver : g_globals[i].version;
        obj_set(nid, g_globals[i].kind, use, 0);
        wlog("bind %s v%u -> id %u", iface, use, nid);
        if (g_globals[i].kind == IF_SHM) {                   // wl_shm.format, once per format
          struct ev e1; ev_begin(&e1, nid, 0); ev_u32(&e1, WL_SHM_ARGB8888); ev_send(&e1);
          struct ev e2; ev_begin(&e2, nid, 0); ev_u32(&e2, WL_SHM_XRGB8888); ev_send(&e2);
        } else if (g_globals[i].kind == IF_SERVER_DECORATION_MANAGER) {
          // GTK3 uses KDE's decoration protocol. AppKit draws the actual frame.
          struct ev e; ev_begin(&e, nid, 0); ev_u32(&e, 2); ev_send(&e); // default_mode(server)
        } else if (g_globals[i].kind == IF_SEAT) {
          struct ev c; ev_begin(&c, nid, 0); ev_u32(&c, 1 | 2); ev_send(&c);   // pointer|keyboard
          if (use >= 2) { struct ev n; ev_begin(&n, nid, 1); ev_str(&n, "default"); ev_send(&n); }
        } else if (g_globals[i].kind == IF_OUTPUT) {
          struct ev g; ev_begin(&g, nid, 0);                 // wl_output.geometry
          ev_u32(&g, 0); ev_u32(&g, 0);                      // x, y
          ev_u32(&g, 300); ev_u32(&g, 200);                  // physical mm
          ev_u32(&g, 0);                                     // subpixel unknown
          ev_str(&g, "rdrift"); ev_str(&g, "shim");          // make, model
          ev_u32(&g, 0);                                     // transform normal
          ev_send(&g);
          struct ev m; ev_begin(&m, nid, 1);                 // wl_output.mode
          ev_u32(&m, 1 | 2);                                 // current | preferred
          ev_u32(&m, 1280); ev_u32(&m, 800); ev_u32(&m, 60000);
          ev_send(&m);
          if (use >= 2) {
            struct ev s; ev_begin(&s, nid, 3); ev_u32(&s, 1); ev_send(&s);     // scale
            struct ev d; ev_begin(&d, nid, 2); ev_send(&d);                    // done
          }
        } else if (g_globals[i].kind == IF_DMABUF) {
          // zwp_linux_dmabuf_v1: advertise the formats the GLES client can allocate, all with
          // the DRM_FORMAT_MOD_LINEAR (0) modifier. v3+ carries modifiers; older clients get
          // the implicit-modifier `format` event instead.
          if (use >= 3) {
            uint32_t formats[2] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888 };
            for (int fi = 0; fi < 2; fi++) {
              struct ev m; ev_begin(&m, nid, 1);              // zwp_linux_dmabuf_v1.modifier
              ev_u32(&m, formats[fi]); ev_u32(&m, 0); ev_u32(&m, 0);
              ev_send(&m);
            }
          } else {
            struct ev f0; ev_begin(&f0, nid, 0); ev_u32(&f0, DRM_FORMAT_ARGB8888); ev_send(&f0); // format
            struct ev f1; ev_begin(&f1, nid, 0); ev_u32(&f1, DRM_FORMAT_XRGB8888); ev_send(&f1);
          }
        }
        return;
      }
      post_error(id, 0, "bind: no such global");
      return;
    }
    return;

  // ------------------------------------------------------------- wl_compositor
  case IF_COMPOSITOR:
    if (op == 0) {                                          // create_surface(new_id)
      uint32_t sid = len >= 4 ? rd32(a) : 0;
      int slot = -1;
      for (int i = 0; i < MAX_SURFACE; i++) if (!g_surf[i].used) { slot = i; break; }
      if (slot < 0) { post_error(id, 0, "too many surfaces"); return; }
      free(g_surf[slot].rgba);            // recycled slot: do not leak its retained pixels
      memset(&g_surf[slot], 0, sizeof g_surf[slot]);
      g_surf[slot].used = 1; g_surf[slot].id = sid;
      obj_set(sid, IF_SURFACE, o->version, (uint32_t)slot);
      // Input focus follows the first surface. A client that goes on to give it an xdg role
      // keeps it (see get_xdg_surface); one that never does — a bare EGL window with no shell —
      // still needs focus, or wl_pointer.enter never fires and motion arrives unannounced.
      if (!g_focus_surface) g_focus_surface = sid;
      wlog("create_surface -> %u", sid);
      return;
    }
    if (op == 1) {                                          // create_region(new_id)
      uint32_t r = len >= 4 ? rd32(a) : 0;
      obj_set(r, IF_REGION, 1, 0);
      return;
    }
    return;

  // ----------------------------------------------------------------- wl_region
  case IF_REGION:
    if (op == 0) { delete_id(id); obj_del(id); }             // destroy
    return;

  // -------------------------------------------------------------------- wl_shm
  case IF_SHM:
    if (op == 0) {                                          // create_pool(new_id, fd, size)
      if (len < 8) return;
      uint32_t pid = rd32(a);
      uint32_t size = rd32(a + 4);                          // the fd is NOT in the byte stream
      int fd = take_fd();
      if (fd < 0) { post_error(id, 0, "create_pool without an fd"); return; }
      int slot = -1;
      for (int i = 0; i < MAX_POOL; i++) if (!g_pool[i].used) { slot = i; break; }
      if (slot < 0) { close(fd); post_error(id, 0, "too many pools"); return; }
      void* m = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
      if (m == MAP_FAILED) { wwarn("mmap pool (%u bytes, fd %d): %s", size, fd, strerror(errno));
                             close(fd); post_error(id, 0, "cannot map pool"); return; }
      g_pool[slot].used = 1; g_pool[slot].fd = fd;
      g_pool[slot].map = (uint8_t*)m; g_pool[slot].size = size;
      obj_set(pid, IF_SHM_POOL, 1, (uint32_t)slot);
      wlog("create_pool %u: fd %d, %u bytes -> slot %d", pid, fd, size, slot);
      return;
    }
    return;

  // --------------------------------------------------------------- wl_shm_pool
  case IF_SHM_POOL: {
    if (o->data >= MAX_POOL) return;
    struct shmpool* p = &g_pool[o->data];
    if (op == 0) {                    // create_buffer(new_id, off, w, h, stride, format)
      if (len < 24) return;
      uint32_t bid = rd32(a);
      int slot = -1;
      for (int i = 0; i < MAX_BUF; i++) if (!g_buf[i].used) { slot = i; break; }
      if (slot < 0) { post_error(id, 0, "too many buffers"); return; }
      g_buf[slot].used   = 1;
      g_buf[slot].pool   = (int)o->data;
      g_buf[slot].offset = (int32_t)rd32(a + 4);
      g_buf[slot].width  = (int32_t)rd32(a + 8);
      g_buf[slot].height = (int32_t)rd32(a + 12);
      g_buf[slot].stride = (int32_t)rd32(a + 16);
      g_buf[slot].format = rd32(a + 20);
      g_buf[slot].id     = bid;
      p->refs++;                                          // the buffer keeps the mapping alive
      obj_set(bid, IF_BUFFER, 1, (uint32_t)slot);
      wlog("create_buffer %u: %dx%d stride %d fmt %u", bid,
           g_buf[slot].width, g_buf[slot].height, g_buf[slot].stride, g_buf[slot].format);
      return;
    }
    if (op == 1) {                                          // destroy
      p->destroyed = 1;
      int slot = (int)o->data;
      delete_id(id); obj_del(id);
      pool_maybe_free(slot);                              // no-op while buffers remain
      return;
    }
    if (op == 2) {                                          // resize(size)
      if (len < 4) return;
      uint32_t ns = rd32(a);
      if (ns <= p->size) return;
      void* m = mmap(NULL, ns, PROT_READ, MAP_SHARED, p->fd, 0);
      if (m == MAP_FAILED) { wwarn("pool resize to %u: %s", ns, strerror(errno)); return; }
      if (p->map) munmap(p->map, p->size);
      p->map = (uint8_t*)m; p->size = ns;
      wlog("pool resized to %u", ns);
      return;
    }
    return;
  }

  // ----------------------------------------------------------------- wl_buffer
  case IF_BUFFER:
    if (op == 0) {                                          // destroy
      int slot = -1;
      if (o->data < MAX_BUF) {
        slot = g_buf[o->data].pool;
        int dmabuf_fd = g_buf[o->data].dmabuf ? g_buf[o->data].dmabuf_fd : -1;
        memset(&g_buf[o->data], 0, sizeof g_buf[0]);
        if (dmabuf_fd >= 0) close(dmabuf_fd);
        if (slot >= 0 && slot < MAX_POOL && g_pool[slot].refs > 0) g_pool[slot].refs--;
      }
      delete_id(id); obj_del(id);
      pool_maybe_free(slot);
    }
    return;

  // ---------------------------------------------------------------- wl_surface
  case IF_SURFACE: {
    struct surface* s = surface_of(id);
    if (!s) return;
    switch (op) {
    case 0: {                                               // destroy
      // Input focus MUST let go of a surface that is about to stop existing, before the object
      // table forgets it. Leaving g_focus_surface pointing at the dead id is the bug that makes
      // a toolkit permanently unclickable after the first popup it shows is dismissed.
      focus_surface_destroyed(id);
      int was_ptr = (g_ptr_surface == id);
      int was_mapped_popup = (s->xdg_popup && s->mapped);
      uint32_t popup_owner = s->popup_owner;
#ifndef WL_HEADLESS
      if (multiwindow_enabled() && s->xdg_popup) rd_secondary_popup_close(s->id);
      if (multiwindow_enabled() && s->xdg_toplevel && s != toplevel_surface())
        rd_secondary_close(s->id);   // also cover clients that skip xdg_toplevel.destroy
#endif
      if (was_ptr) {                       // silently: its id is dead, so no leave may name it
        seatres_clear_entered(g_ptrs, g_nptr);
        g_ptr_surface = 0; g_sent_x = g_sent_y = INT32_MIN;
      }
      if (g_cursor_surface == id) { g_cursor_surface = 0; recomposite(); }
      int was_subsurface = s->is_subsurface;
      s->used = 0; s->mapped = 0; s->xdg_popup = 0;
      s->is_subsurface = 0; s->subsurface_parent = 0;
      free(s->rgba); s->rgba = NULL; s->rgba_cap = 0; s->rgba_w = s->rgba_h = 0;
      if (was_subsurface) recomposite();
      delete_id(id); obj_del(id);
      if (was_mapped_popup) repaint_popup_owner(popup_owner);
      if (was_ptr && !was_mapped_popup) ptr_goto(g_ptr_x, g_ptr_y);
      return;
    }
    case 1:                                                 // attach(buffer, x, y)
      s->pending_buffer = len >= 4 ? rd32(a) : 0;
      s->has_pending_buffer = 1;
      return;
    case 2: case 9:                                         // damage / damage_buffer
      return;
    case 3: {                                               // frame(new_id callback)
      uint32_t cb = len >= 4 ? rd32(a) : 0;
      obj_set(cb, IF_CALLBACK, 1, 0);
      if (s->nframe < MAX_FRAME) s->frame_cb[s->nframe++] = cb;
      return;
    }
    case 4: case 5:                                         // set_opaque_region / set_input_region
      return;
    case 6:                                                 // commit
      surface_commit(s);
      return;
    case 7: case 8: case 10:                                // transform / scale / offset
      return;
    default:
      return;
    }
  }

  // ------------------------------------------------------------------- wl_seat
  case IF_SEAT:
    if (op == 0) {                                          // get_pointer(new_id)
      uint32_t p = len >= 4 ? rd32(a) : 0;
      obj_set(p, IF_POINTER, o->version, 0);
      seatres_add(g_ptrs, &g_nptr, p, o->version);
      wlog("get_pointer -> %u (v%u)  [%d wl_pointer object(s) now live]", p, o->version, g_nptr);
      return;
    }
    if (op == 1) {                                          // get_keyboard(new_id)
      uint32_t k = len >= 4 ? rd32(a) : 0;
      obj_set(k, IF_KEYBOARD, o->version, 0);
      seatres_add(g_kbds, &g_nkbd, k, o->version);
      wlog("get_keyboard -> %u (v%u)  [%d wl_keyboard object(s) now live]", k, o->version, g_nkbd);
      // A keyboard MUST be handed a keymap immediately, and it has to be a REAL one: format 0
      // ("no keymap") is legal by the letter of the protocol but clients do not tolerate it —
      // mesa-demos' eglut asserts outright,
      //     assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
      // and dies. So send XKB_V1 and let libxkbcommon in the guest resolve the includes against
      // the xkeyboard-config data already in the rootfs (/usr/share/X11/xkb).
      static const char kKeymap[] =
        "xkb_keymap {\n"
        "  xkb_keycodes { include \"evdev\" };\n"
        "  xkb_types    { include \"complete\" };\n"
        "  xkb_compat   { include \"complete\" };\n"
        "  xkb_symbols  { include \"pc+us\" };\n"
        "};\n";
      const size_t klen = sizeof kKeymap;                    // INCLUDING the NUL, as clients expect
      char tmpl[] = "/tmp/rdwlkeymapXXXXXX";
      int fd = mkstemp(tmpl);
      if (fd >= 0) {
        unlink(tmpl);
        if (write(fd, kKeymap, klen) == (ssize_t)klen) {
          struct ev e; ev_begin(&e, k, 0);                   // wl_keyboard.keymap
          ev_u32(&e, 1);                                     // format = XKB_V1
          ev_u32(&e, (uint32_t)klen);
          ev_send_fd(&e, fd);
        }
        close(fd);
      }
      if (o->version >= 4) {
        struct ev r; ev_begin(&r, k, 5); ev_u32(&r, 25); ev_u32(&r, 400); ev_send(&r);  // repeat
      }
      return;
    }
    if (op == 2) {                                          // get_touch(new_id)
      uint32_t t = len >= 4 ? rd32(a) : 0;
      obj_set(t, IF_TOUCH, o->version, 0);
      return;
    }
    if (op == 3) { delete_id(id); obj_del(id); }             // release
    return;

  // wl_pointer: op 0 = set_cursor, op 1 = release. wl_keyboard/wl_touch: op 0 = release.
  // A released resource must leave the broadcast set (see seatres_del).
  case IF_POINTER:
    if (op == 0) {                                       // set_cursor(serial, surface, hotspot_x, hotspot_y)
      uint32_t serial = len >= 4 ? rd32(a) : 0;
      uint32_t sid    = len >= 8 ? rd32(a + 4) : 0;
      int32_t  hot_x  = len >= 12 ? (int32_t)rd32(a + 8) : 0;
      int32_t  hot_y  = len >= 16 ? (int32_t)rd32(a + 12) : 0;
      (void)serial;
      g_cursor_surface = surface_alive(sid) ? sid : 0;   // surface 0 (or a dead id) hides the cursor
      g_cursor_hot_x = hot_x; g_cursor_hot_y = hot_y;
      wlog("set_cursor surface %u hotspot (%d,%d)", g_cursor_surface, hot_x, hot_y);
      if (g_cursor_surface) recomposite();               // the cursor may have appeared/changed
    }
    if (op == 1) { seatres_del(g_ptrs, &g_nptr, id); delete_id(id); obj_del(id); }
    return;
  case IF_KEYBOARD:
    if (op == 0) { seatres_del(g_kbds, &g_nkbd, id); delete_id(id); obj_del(id); }
    return;
  case IF_TOUCH:
    if (op == 0) { delete_id(id); obj_del(id); }
    return;

  // ----------------------------------------------------------------- wl_output
  case IF_OUTPUT:
    if (op == 0) { delete_id(id); obj_del(id); }             // release
    return;

  // --------------------------------------------------------------- xdg_wm_base
  case IF_SERVER_DECORATION_MANAGER:
    if (op == 0 && len >= 8) { // create(new_id, wl_surface)
      uint32_t did = rd32(a), sid = rd32(a + 4);
      if (!surface_of(sid)) { post_error(id, 0, "decoration for unknown surface"); return; }
      obj_set(did, IF_SERVER_DECORATION, 1, sid);
      struct ev e; ev_begin(&e, did, 0); ev_u32(&e, 2); ev_send(&e); // mode(server)
    }
    return;

  case IF_SERVER_DECORATION:
    if (op == 0) { delete_id(id); obj_del(id); return; } // release
    if (op == 1) { // request_mode; AppKit owns the title bar in this compositor
      struct ev e; ev_begin(&e, id, 0); ev_u32(&e, 2); ev_send(&e);
    }
    return;

  // --------------------------------------------------------------- xdg_wm_base
  case IF_WM_BASE:
    if (op == 0) { delete_id(id); obj_del(id); return; }     // destroy
    if (op == 1) {                                           // create_positioner(new_id)
      uint32_t p = len >= 4 ? rd32(a) : 0;
      int slot = -1;
      for (int i = 0; i < MAX_POS; i++) if (!g_pos[i].used) { slot = i; break; }
      if (slot < 0) { obj_set(p, IF_XDG_POSITIONER, o->version, MAX_POS); return; }
      // MEMSET: a recycled slot must not inherit the previous popup's anchor rect or gravity,
      // which would place this menu wherever the last one went.
      memset(&g_pos[slot], 0, sizeof g_pos[slot]);
      g_pos[slot].used = 1; g_pos[slot].w = 1; g_pos[slot].h = 1;
      obj_set(p, IF_XDG_POSITIONER, o->version, (uint32_t)slot);
      return;
    }
    if (op == 2) {                                           // get_xdg_surface(new_id, surface)
      if (len < 8) return;
      uint32_t xid = rd32(a), sid = rd32(a + 4);
      struct surface* s = surface_of(sid);
      if (!s) { post_error(id, 0, "get_xdg_surface on a non-surface"); return; }
      s->xdg_surface = xid;
      // Focus is NOT claimed here. An xdg_surface is only half a role — it becomes a window at
      // get_toplevel and a MENU at get_popup, and a toolkit creates one of these for every
      // tooltip, popover and combo list it shows. Handing focus to each of them, then never
      // handing it back when the popup's wl_surface is destroyed, leaves g_focus_surface naming
      // a dead object for the rest of the session; from then on every enter/motion/button is
      // addressed to an id the client resolves to NULL and discards. The window carries on
      // rendering and nothing is clickable ever again. Focus is settled in get_toplevel instead.
      obj_set(xid, IF_XDG_SURFACE, o->version, (uint32_t)(s - g_surf));
      wlog("get_xdg_surface %u for surface %u", xid, sid);
      return;
    }
    if (op == 3) return;                                     // pong
    return;

  // ------------------------------------------------------------- xdg_surface
  case IF_XDG_SURFACE: {
    if (o->data >= MAX_SURFACE) return;
    struct surface* s = &g_surf[o->data];
    if (op == 0) { delete_id(id); obj_del(id); return; }      // destroy
    if (op == 1) {                                            // get_toplevel(new_id)
      uint32_t tid = len >= 4 ? rd32(a) : 0;
      s->xdg_toplevel = tid;
      s->is_popup = 0;
      s->acked = 0;                      // a new role starts a new configure sequence; see get_popup
      obj_set(tid, IF_XDG_TOPLEVEL, o->version, o->data);
      // THIS is the surface that owns the window, and the only one input may be addressed to.
      // A toolkit's first surface is often not the toplevel, so the role-less default claimed in
      // create_surface has to be overridden here — but by a toplevel, never by a popup.
      g_toplevel_surface = s->id;
      set_focus_surface(s->id);
      wlog("get_toplevel %u (surface %u)", tid, s->id);
      return;
    }
    if (op == 2) {                                            // get_popup(new_id, parent, positioner)
      uint32_t pid = len >= 4 ? rd32(a) : 0;
      uint32_t parent_xdg = len >= 8 ? rd32(a + 4) : 0;
      uint32_t posid = len >= 12 ? rd32(a + 8) : 0;
      s->is_popup = 1;
      s->xdg_popup = pid;
      s->popup_owner = 0;
      s->stack = g_stack_next++;
      s->mapped = 0;
      // A NEW ROLE STARTS A NEW CONFIGURE SEQUENCE. GTK reuses one wl_surface for successive
      // popups — the second menu is the same surface 30 as the first — and `acked` left over from
      // the previous role makes surface_commit skip the configure, so the client waits forever for
      // an xdg_surface.configure that never comes and the menu never opens a second time. Measured
      // as exactly that: get_popup logged, no buffer ever committed, the app silent thereafter.
      s->acked = 0;

      struct positioner dflt; memset(&dflt, 0, sizeof dflt);
      struct object* po = obj_get(posid);
      const struct positioner* p =
        (po && po->iface == IF_XDG_POSITIONER && po->data < MAX_POS) ? &g_pos[po->data] : &dflt;
      s->pw = p->w > 0 ? p->w : 1;
      s->ph = p->h > 0 ? p->h : 1;
      popup_place(s, p);                                      // anchor + gravity + offset

      // The anchor rect is expressed against the PARENT's geometry, so a submenu is additionally
      // offset by wherever its parent popup landed.
      int32_t base_x = 0, base_y = 0;
      struct object* pa = obj_get(parent_xdg);
      if (pa && pa->iface == IF_XDG_SURFACE && pa->data < MAX_SURFACE) {
        struct surface* ps = &g_surf[pa->data];
        if (ps->xdg_popup) {
          base_x = ps->px; base_y = ps->py;
          s->popup_owner = ps->popup_owner;
        } else if (ps->xdg_toplevel) {
          s->popup_owner = ps->id;
        }
      }
      if (!s->popup_owner && toplevel_surface()) s->popup_owner = toplevel_surface()->id;
      s->px = base_x + s->rx; s->py = base_y + s->ry;

      obj_set(pid, IF_XDG_POPUP, o->version, o->data);
      // A popup MUST be configured before its xdg_surface is, and the client waits for that
      // configure before it will map: xdg_popup.configure carries the geometry the compositor is
      // granting. An unanswered popup is a toolkit that stops responding, not merely a menu that
      // is missing. (The paired one in the configure sequence is sent from surface_commit.)
      struct ev c; ev_begin(&c, pid, 0);                      // xdg_popup.configure(x,y,w,h)
      ev_u32(&c, (uint32_t)s->rx); ev_u32(&c, (uint32_t)s->ry);
      ev_u32(&c, (uint32_t)s->pw); ev_u32(&c, (uint32_t)s->ph);
      ev_send(&c);
      wlog("get_popup %u on surface %u -> rel %d,%d abs %d,%d size %dx%d",
           pid, s->id, s->rx, s->ry, s->px, s->py, s->pw, s->ph);
      return;
    }
    if (op == 3) return;                                      // set_window_geometry
    if (op == 4) { s->acked = 1; return; }                    // ack_configure
    return;
  }

  // ------------------------------------------------------------ xdg_toplevel
  case IF_XDG_TOPLEVEL:
    if ((op == 5 || op == 6) && o->data < MAX_SURFACE &&
        getenv("RDWL_CLIENT_DECORATIONS")) {                 // move / resize(seat, serial, edges)
      struct surface* s = &g_surf[o->data];
      uint32_t seat = len >= 4 ? rd32(a) : 0;
      uint32_t serial = len >= 8 ? rd32(a + 4) : 0;
      uint32_t edge = op == 6 && len >= 12 ? rd32(a + 8) : 0;
      struct object* seat_object = obj_get(seat);
      struct surface* primary = toplevel_surface();
      uint32_t host_window = primary == s ? 0 : s->id;
      if (!s->xdg_toplevel || !seat_object || seat_object->iface != IF_SEAT ||
          !g_interactive_pressed || !serial || serial != g_interactive_serial ||
          g_interactive_owner != s->id || g_ptr_host_window != host_window ||
          (op == 6 && edge != 1 && edge != 2 && edge != 4 && edge != 5 &&
           edge != 6 && edge != 8 && edge != 9 && edge != 10)) {
        wlog("ignored invalid interactive %s for toplevel %u", op == 5 ? "move" : "resize", id);
        return;
      }
#ifndef WL_HEADLESS
      if (rd_guest_interactive(host_window, op == 6 ? edge : 0, g_ptr_x, g_ptr_y) == 0)
        wlog("interactive %s accepted for toplevel %u serial %u", op == 5 ? "move" : "resize", id, serial);
#endif
      return;
    }
    if (op == 1) {                                             // set_parent(toplevel)
      if (o->data < MAX_SURFACE) {
        uint32_t parent = len >= 4 ? rd32(a) : 0;
        struct object* po = obj_get(parent);
        g_surf[o->data].parent_surface =
          po && po->iface == IF_XDG_TOPLEVEL && po->data < MAX_SURFACE ? g_surf[po->data].id : 0;
      }
      return;
    }
    // destroy — a DIALOG closing. This used to delete the object id and nothing else, so the
    // surface stayed `mapped` with its pixels and recomposite() kept blending them forever.
    // The identical reasoning is already written down one case below for xdg_popup ("repaint
    // without it, or its pixels stay on screen forever"); it was simply never extended to the
    // secondary-toplevel path that landed later.
    //
    // MEASURED on Qt Designer's startup "New Form" dialog: the Close button fires, the client
    // sends xdg_toplevel@22.destroy + xdg_surface@21.destroy, and from that line to the end of
    // the run the compositor presents ZERO further frames. Nothing else will ever damage the
    // parent — from the client's point of view closing a child window does not dirty it — so if
    // the compositor does not repaint here, no one does. The dialog is gone in the client's model
    // and painted on screen permanently: every screenshot, every OCR check and every human
    // looking at the window sees a dialog that no longer exists. An instrument reading that frame
    // reports the click as having done nothing, which is the exact inverse of the truth.
    if (op == 0) {
      if (o->data < MAX_SURFACE) {
        struct surface* s = &g_surf[o->data];
        // Order matters: toplevel_surface() finds the first surface still holding the role, so
        // it must be asked BEFORE the role is cleared or it answers about a different surface.
        int was_secondary = s->mapped && s != toplevel_surface();
        // A destroyed dialog must not keep keyboard focus. get_toplevel() points
        // g_toplevel_surface at every toplevel it sees, the dialog included, so without this the
        // focus fallback target is itself a dead surface and the app cannot be typed into again.
        if (g_toplevel_surface == s->id) {
          struct surface* primary = toplevel_surface();
          g_toplevel_surface = (primary && primary != s) ? primary->id : 0;
        }
        s->xdg_toplevel = 0; s->mapped = 0;
#ifndef WL_HEADLESS
        if (was_secondary && multiwindow_enabled()) rd_secondary_close(s->id);
#endif
        delete_id(id); obj_del(id);
        // Only a SECONDARY needs this. If the primary is going away the window is closing, and
        // recomposite() has no toplevel left to build a frame from anyway.
        if (was_secondary) {
          struct surface* primary = toplevel_surface();
          if (primary && primary->title[0]) gpuwin_set_title(primary->title);
          recomposite();
          if (!multiwindow_enabled()) ptr_goto(g_ptr_x, g_ptr_y);
          else if (g_ptr_host_window == s->id) {
            // The last coordinates belonged to the closing child, not the
            // primary. Wait for a real event from another host window.
            ptr_leave();
            g_ptr_host_window = 0;
            g_ptr_x = g_ptr_y = INT32_MIN;
          }
        }
        return;
      }
      delete_id(id); obj_del(id); return;
    }
    // WHICH REQUESTS ACTUALLY ARRIVE. set_title was wired and never fired, and with no logging
    // on this path "the branch did not run" and "the client never sent it" look identical.
    if (getenv("RDVERBOSE")) fprintf(stderr, "[wl] xdg_toplevel op=%u len=%zu\n", op, (size_t)len);
    if (op == 2) {                                             // set_title
      size_t off = 0; const char* t = arg_str(a, len, &off);
      // Was logged and thrown away, which is why every Linux app shared one hardcoded macOS
      // window title. The client is telling us its name; use it.
      if (t) {
        wlog("title: %s", t);
        if (o->data < MAX_SURFACE) snprintf(g_surf[o->data].title, sizeof g_surf[o->data].title, "%s", t);
        if (!multiwindow_enabled() || o->data >= MAX_SURFACE ||
            &g_surf[o->data] == toplevel_surface()) gpuwin_set_title(t);
      }
      return;
    }
    if (op == 3) {                                             // set_app_id
      size_t off = 0; const char* t = arg_str(a, len, &off);
      // The Dock tile and menu-bar name of a NON-BUNDLED process come from the process name,
      // so this is what stops a Linux app being listed as "shim_gui".
      if (t) { wlog("app_id: %s", t); gpuwin_set_appname(t); }
      return;
    }
    if (op == 13) {
      if (!multiwindow_enabled() || o->data >= MAX_SURFACE ||
          &g_surf[o->data] == toplevel_surface()) gpuwin_minimize();
      return;
    }                                                         // set_minimized
    return;                                                    // everything else is advisory

  // ----------------------------------------------------------- xdg_positioner
  case IF_XDG_POSITIONER: {
    if (o->data >= MAX_POS) return;
    struct positioner* p = &g_pos[o->data];
    // Parsing only set_size gives a popup a size and no POSITION, so every menu lands in the
    // top-left corner regardless of the button that opened it. anchor/gravity/offset are what
    // decide where it actually goes, and hit-testing then follows the rect we chose.
    switch (op) {
    case 0: p->used = 0; delete_id(id); obj_del(id); return;   // destroy
    case 1: if (len >= 8)  { p->w  = (int32_t)rd32(a);     p->h  = (int32_t)rd32(a + 4); } return;
    case 2: if (len >= 16) { p->ax = (int32_t)rd32(a);     p->ay = (int32_t)rd32(a + 4);
                             p->aw = (int32_t)rd32(a + 8); p->ah = (int32_t)rd32(a + 12); } return;
    case 3: if (len >= 4)  p->anchor  = rd32(a); return;       // set_anchor
    case 4: if (len >= 4)  p->gravity = rd32(a); return;       // set_gravity
    case 6: if (len >= 8)  { p->ox = (int32_t)rd32(a);     p->oy = (int32_t)rd32(a + 4); } return;
    default: return;                            // constraint_adjustment and reactive are advisory
    }
  }

  // ----------------------------------------------------------------- xdg_popup
  case IF_XDG_POPUP:
    // A destructor request that the server never answers with delete_id strands the id in the
    // client's map forever; ids are cheap but the leak is a real protocol debt, and the object
    // staying alive server-side means a later request on a REUSED id is dispatched as a popup.
    if (op == 0) {                                             // destroy
      if (o->data < MAX_SURFACE) {
        struct surface* s = &g_surf[o->data];
        int was_mapped = s->mapped;
        uint32_t popup_owner = s->popup_owner;
#ifndef WL_HEADLESS
        if (multiwindow_enabled()) rd_secondary_popup_close(s->id);
#endif
        s->xdg_popup = 0; s->mapped = 0;
        delete_id(id); obj_del(id);
        // The menu just stopped existing. Repaint without it, or its pixels stay on screen
        // forever, and re-hit-test so the pointer enters whatever was underneath.
        if (was_mapped) repaint_popup_owner(popup_owner);
        return;
      }
      delete_id(id); obj_del(id);
    }
    // grab(seat, serial) is op 1. We take no real seat grab; popup_dismiss_if_outside() on the
    // button path provides the part clients actually depend on, which is popup_done.
    return;

  case IF_DDM:
    if (op == 0 && len >= 4) { // create_data_source(new_id)
      uint32_t sid = rd32(a);
      for (int i = 0; i < MAX_DATA_SOURCE; i++) if (!g_sources[i].id) {
        memset(&g_sources[i], 0, sizeof g_sources[i]);
        g_sources[i].id = sid;
        obj_set(sid, IF_DATA_SOURCE, o->version, (uint32_t)i);
        return;
      }
      post_error(id, 0, "too many clipboard sources");
    }
    if (op == 1 && len >= 8) { // get_data_device(new_id, seat)
      uint32_t did = rd32(a);
      obj_set(did, IF_DATA_DEVICE, o->version, 0);
      if (g_ndevices < 16) g_data_devices[g_ndevices++] = did;
      if (g_selection_source) send_selection_to(did);
    }
    return;

  case IF_DATA_SOURCE:
    if (o->data >= MAX_DATA_SOURCE) return;
    if (op == 0) { // offer(mime)
      size_t off = 0; const char* mime = arg_str(a, len, &off);
      struct data_source* src = &g_sources[o->data];
      if (mime && src->nmime < 16) {
        snprintf(src->mime[src->nmime++], sizeof src->mime[0], "%s", mime);
      }
    } else if (op == 1) { // destroy
      if (g_selection_source == id) g_selection_source = 0;
      memset(&g_sources[o->data], 0, sizeof g_sources[0]);
      delete_id(id); obj_del(id);
    }
    return; // set_actions is advisory for clipboard

  case IF_DATA_DEVICE:
    if (op == 1 && len >= 8) { // set_selection(source, serial)
      g_selection_source = rd32(a);
      for (int i = 0; i < g_ndevices; i++) send_selection_to(g_data_devices[i]);
      if (g_selection_source) {
        atomic_store(&g_guest_pasteboard_count, gpuwin_clipboard_count());
        export_selection_to_mac();
      }
    } else if (op == 2) { // release
      for (int i = 0; i < g_ndevices; i++) if (g_data_devices[i] == id) {
        memmove(&g_data_devices[i], &g_data_devices[i + 1],
                (size_t)(--g_ndevices - i) * sizeof g_data_devices[0]);
        break;
      }
      delete_id(id); obj_del(id);
    }
    return;

  case IF_DATA_OFFER:
    if (op == 1) { // receive(mime, fd): ask the owning GTK source to write it
      size_t off = 0; const char* mime = arg_str(a, len, &off);
      int fd = take_fd();
      if (fd < 0) return;
      if (mime && source_by_id(o->data)) {
        struct ev e; ev_begin(&e, o->data, 1); ev_str(&e, mime);
        ev_send_fd(&e, fd);
      } else if (mime && !o->data) {
        char* data = malloc(1u << 20);
        if (data) {
          int n = gpuwin_clipboard_read(mime, data, 1u << 20);
          if (n > 0) {
            int off = 0;
            while (off < n) { ssize_t w = write(fd, data + off, (size_t)(n - off));
              if (w <= 0) break; off += (int)w; }
          }
          free(data);
        }
      }
      close(fd);
    } else if (op == 2) { obj_del(id); } // destroy (server-created id)
    return;

  // ----------------------------------------------- zwp_linux_dmabuf_v1 / params
  case IF_DMABUF:
    if (op == 0) {                                          // destroy
      delete_id(id); obj_del(id);
      return;
    }
    if (op == 1) {                                          // create_params(new_id)
      if (len < 4) return;
      uint32_t pid = rd32(a);
      int slot = -1;
      for (int i = 0; i < MAX_DMABUF_PARAMS; i++) if (!g_dmabuf_params[i].used) { slot = i; break; }
      if (slot < 0) { post_error(id, 0, "too many dmabuf params"); return; }
      memset(&g_dmabuf_params[slot], 0, sizeof g_dmabuf_params[slot]);
      g_dmabuf_params[slot].used = 1;
      g_dmabuf_params[slot].fd = -1;
      obj_set(pid, IF_DMABUF_PARAMS, o->version, (uint32_t)slot);
      wlog("zwp_linux_dmabuf_v1.create_params -> %u", pid);
      return;
    }
    if (op == 2) {                                          // get_default_feedback(new_id)
      if (len < 4) return;
      uint32_t fid = rd32(a);
      obj_set(fid, IF_DMABUF_FEEDBACK, o->version, 0);
      // zwp_linux_dmabuf_feedback_v1 event opcodes: done=0, format_table=1, main_device=2.
      // main_device (op 2): dev_t 226:128 (renderD128), 8-byte aarch64 little-endian layout.
      uint8_t devt[8] = { 0x80, 0xE2, 0, 0, 0, 0, 0, 0 };
      struct ev m; ev_begin(&m, fid, 2); ev_array(&m, devt, sizeof devt); ev_send(&m);
      // format_table (op 1): fd (out-of-band via SCM_RIGHTS, NO body slot — same convention as
      // wl_keyboard.keymap) + size, to the packed {format,pad,modifier}[] table.
      int tfd = dmabuf_format_table_fd();
      if (tfd >= 0) {
        struct ev f; ev_begin(&f, fid, 1);            // op 1 = format_table
        wr32(f.b + f.n, 32); f.n += 4;                // size (2 entries * 16 bytes)
        ev_send_fd(&f, tfd);
        close(tfd);
      } else {
        wwarn("get_default_feedback: cannot build format table fd");
      }
      // The feedback's format map is built from the TRANCHES, not from format_table alone:
      // KWin's LinuxDmabufFeedbackV1 populates m_formats only inside tranche_formats (and only
      // when the tranche targets the main device). Send one tranche covering both table entries,
      // otherwise KWin sees an empty format list and its EGL swapchain reports
      // "Could not find a suitable render format". Event opcodes: tranche_target_device=4,
      // tranche_flags=6, tranche_formats=5, tranche_done=3.
      {
        struct ev tt; ev_begin(&tt, fid, 4); ev_array(&tt, devt, sizeof devt); ev_send(&tt);
        uint16_t idx[2] = { 0, 1 };
        struct ev tf; ev_begin(&tf, fid, 5); ev_array(&tf, idx, sizeof idx); ev_send(&tf);
        struct ev tg; ev_begin(&tg, fid, 6); ev_u32(&tg, 0); ev_send(&tg);   // flags: no SCANOUT
        struct ev td; ev_begin(&td, fid, 3); ev_send(&td);                   // tranche_done
      }
      // done (op 0): sent last, after all feedback parameters.
      struct ev d; ev_begin(&d, fid, 0); ev_send(&d);
      wlog("zwp_linux_dmabuf_v1.get_default_feedback -> %u (main_device 226:128 + format_table + done)", fid);
      return;
    }
    return;

  case IF_DMABUF_FEEDBACK:
    if (op == 0) {                                          // destroy
      delete_id(id); obj_del(id);
      return;
    }
    return;

  case IF_DMABUF_PARAMS: {
    if (o->data >= MAX_DMABUF_PARAMS) return;
    struct dmabuf_params* p = &g_dmabuf_params[o->data];
    if (op == 0) {                                          // destroy
      if (p->fd >= 0) close(p->fd);
      memset(p, 0, sizeof *p);
      delete_id(id); obj_del(id);
      return;
    }
    if (op == 1) {                                          // add(fd, plane, offset, stride, mod_hi, mod_lo)
      if (len < 20) return;
      int fd = take_fd();                                   // the fd is out of band (SCM_RIGHTS)
      uint32_t plane = rd32(a), offset = rd32(a + 4), stride = rd32(a + 8);
      uint32_t mod_hi = rd32(a + 12), mod_lo = rd32(a + 16);
      if (fd < 0 || !p->used || p->have) {
        if (fd >= 0) close(fd);
        wwarn("zwp_linux_buffer_params_v1.add rejected (fd=%d plane=%u)", fd, plane);
        return;
      }
      p->fd = fd; p->have = 1; p->stride = stride; p->offset = offset;
      wlog("zwp_linux_buffer_params_v1.add fd=%d stride=%u mod=%u:%u",
           fd, stride, mod_hi, mod_lo);
      return;
    }
    if (op == 2) {                                          // create(w,h,format,flags) — async
      // No async import path yet: fail, which lets Mesa fall back to wl_shm.
      if (p->fd >= 0) { close(p->fd); p->fd = -1; }
      p->have = 0;
      struct ev e; ev_begin(&e, id, 1); ev_send(&e);        // zwp_linux_buffer_params_v1.failed
      wlog("zwp_linux_buffer_params_v1.create -> failed (async import not implemented)");
      return;
    }
    if (op == 3) {                                          // create_immed(id, w, h, format, flags)
      if (len < 20) return;
      uint32_t buffer_id = rd32(a);
      int32_t width = (int32_t)rd32(a + 4), height = (int32_t)rd32(a + 8);
      uint32_t format = rd32(a + 12), flags = rd32(a + 16);
      if (!p->used || !p->have || width <= 0 || height <= 0) {
        post_error(id, 5 /* INVALID_DIMENSIONS */, "dmabuf create_immed: no plane or bad size");
        return;
      }
      int fd = p->fd;
      // Resolve the imported fd to an opaque GEM handle through the shim's DRM render node.
      // The real IOSurface backing is wired in a separate task; here we only prove the ioctl path.
      int render_fd = dmabuf_render_fd();
      struct drm_prime_handle ph = { 0, 0, fd };
      long r = drm_ioctl(render_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, (unsigned long)&ph);
      uint32_t handle = ph.handle;
      if (r == 0) {
        struct drm_gem_close gc = { handle, 0 };
        drm_ioctl(render_fd, DRM_IOCTL_GEM_CLOSE, (unsigned long)&gc);
      }
      fprintf(stderr, "WLSRV_DMABUF_IMPORT fd=%d handle=%u ioctl=%ld size=%dx%d stride=%u format=0x%08x flags=0x%x\n",
              fd, handle, r, width, height, p->stride, format, flags);
      int slot = -1;
      for (int i = 0; i < MAX_BUF; i++) if (!g_buf[i].used) { slot = i; break; }
      if (slot < 0) { post_error(id, 0, "too many buffers"); return; }
      memset(&g_buf[slot], 0, sizeof g_buf[slot]);
      g_buf[slot].used = 1;
      g_buf[slot].pool = -1;                                // no shm pool behind this buffer
      g_buf[slot].dmabuf = 1;
      g_buf[slot].dmabuf_fd = fd;
      g_buf[slot].dmabuf_handle = handle;
      g_buf[slot].width = width;
      g_buf[slot].height = height;
      g_buf[slot].stride = (int32_t)p->stride;
      g_buf[slot].format = format;
      g_buf[slot].id = buffer_id;
      obj_set(buffer_id, IF_BUFFER, 1, (uint32_t)slot);
      p->fd = -1; p->have = 0;                              // fd ownership moved to the buffer
      wlog("zwp_linux_buffer_params_v1.create_immed -> wl_buffer %u (dmabuf)", buffer_id);
      return;
    }
    return;
  }

  // ------------------------------------------- wp_single_pixel_buffer_manager_v1
  case IF_SINGLE_PIXEL_MANAGER:
    if (op == 0) { delete_id(id); obj_del(id); return; }
    if (op == 1) {                                          // create_u32_rgba_buffer(new_id, r,g,b,a)
      if (len < 20) return;
      uint32_t bid = rd32(a);
      uint32_t r = rd32(a + 4), g = rd32(a + 8), b = rd32(a + 12), al = rd32(a + 16);
      // The one RGBA u32 is stored in the object's `data` slot, packed so the BGRA bytes fall
      // out in memory order: (a<<24)|(r<<16)|(g<<8)|b. fill_single_pixel() unpacks it.
      uint32_t packed = ((al & 0xff) << 24) | ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
      obj_set(bid, IF_SINGLE_PIXEL_BUFFER, 1, packed);
      wlog("create_u32_rgba_buffer -> %u (r=%u g=%u b=%u a=%u)", bid, r, g, b, al);
      return;
    }
    return;
  case IF_SINGLE_PIXEL_BUFFER:
    if (op == 0) { delete_id(id); obj_del(id); return; }    // destroy
    return;

  // ------------------------------------------------------------- wp_viewporter
  case IF_VIEWPORTER:
    if (op == 0) { delete_id(id); obj_del(id); return; }
    if (op == 1) {                                          // get_viewport(new_id, surface)
      if (len < 8) return;
      uint32_t vid = rd32(a), sid = rd32(a + 4);
      struct surface* vs = surface_of(sid);
      // Remember WHICH surface this viewport scales; set_destination needs to reach it.
      obj_set(vid, IF_VIEWPORT, 1, vs ? (uint32_t)(vs - g_surf) : MAX_SURFACE);
      return;
    }
    return;
  case IF_VIEWPORT:
    if (op == 0) { delete_id(id); obj_del(id); return; }    // destroy
    // set_source (op 1, fixed-point crop) stays advisory. set_destination (op 2, integer size)
    // is what gives a buffer-less surface (a single-pixel buffer) its logical size.
    if (op == 2 && len >= 8 && o->data < MAX_SURFACE) {
      struct surface* vs = &g_surf[o->data];
      vs->vp_w = (int32_t)rd32(a);
      vs->vp_h = (int32_t)rd32(a + 4);
      wlog("viewport set_destination surface %u -> %dx%d", vs->id, vs->vp_w, vs->vp_h);
    }
    return;

  // ------------------------------------------ zwp_pointer_constraints_v1
  case IF_POINTER_CONSTRAINTS:
    if (op == 0) { delete_id(id); obj_del(id); return; }
    if (op == 1) {                                          // lock_pointer(new_id, surface, pointer, region, lifetime)
      if (len < 20) return;
      uint32_t lid = rd32(a);
      obj_set(lid, IF_LOCKED_POINTER, 1, 0);
      struct ev e; ev_begin(&e, lid, 0); ev_send(&e);       // locked()
      return;
    }
    if (op == 2) {                                          // confine_pointer(new_id, surface, pointer, region, lifetime)
      if (len < 20) return;
      uint32_t cid = rd32(a);
      obj_set(cid, IF_CONFINED_POINTER, 1, 0);
      struct ev e; ev_begin(&e, cid, 0); ev_send(&e);       // confined()
      return;
    }
    return;
  case IF_LOCKED_POINTER:
  case IF_CONFINED_POINTER:
    if (op == 0) { delete_id(id); obj_del(id); return; }    // destroy
    return;

  // -------------------------------------------------------- wp_presentation_time
  case IF_PRESENTATION:
    if (op == 0) { delete_id(id); obj_del(id); return; }
    if (op == 1) {                                          // feedback(surface, new_id callback)
      if (len < 8) return;
      uint32_t cb = rd32(a + 4);
      obj_set(cb, IF_PRESENTATION_FEEDBACK, 1, 0);
      // Answer immediately: the nested compositor's render loop blocks on frame feedback.
      //
      // The timestamp MUST be a valid CLOCK_MONOTONIC value in the SAME time base the guest's
      // std::chrono::steady_clock reads (Linux CLOCK_MONOTONIC -> host CLOCK_UPTIME_RAW; see
      // clockid_l2m in syscalls.c). KWin's WaylandOutput::framePresented() subtracts the
      // wl_callback.done time from this timestamp to derive the presentation safety margin, and
      // RenderLoop::notifyVblank() records it as lastPresentationTimestamp. A hardcoded 0 leaves
      // lastPresentationTimestamp at 0 forever, so every later scheduleRepaint() computes
      // pageflipsSince = (now-0)/vblank = huge and mis-schedules the compositor. Sending the real
      // monotonic time keeps the nested render loop on an honest vblank cadence.
      struct timespec now;
      clock_gettime(CLOCK_UPTIME_RAW, &now);
      uint64_t sec = (uint64_t)now.tv_sec;
      struct ev e; ev_begin(&e, cb, 1);                     // wp_presentation_feedback.presented
      ev_u32(&e, (uint32_t)(sec >> 32));                    // tv_sec_hi
      ev_u32(&e, (uint32_t)(sec & 0xffffffffu));            // tv_sec_lo
      ev_u32(&e, (uint32_t)now.tv_nsec);                    // tv_nsec
      ev_u32(&e, 16666666);                                 // refresh_nsec (60 Hz)
      ev_u32(&e, 0); ev_u32(&e, 1);                         // seq_hi, seq_lo
      ev_u32(&e, 0);                                        // flags
      ev_send(&e);
      return;
    }
    return;
  case IF_PRESENTATION_FEEDBACK:
    return;

  // ----------------------------------------------------------- wl_subcompositor
  case IF_SUBCOMPOSITOR:
    if (op == 0) { delete_id(id); obj_del(id); return; }    // destroy
    if (op == 1) {                                          // get_subsurface(new_id, surface, parent)
      if (len < 12) return;
      uint32_t nid = rd32(a), sid = rd32(a + 4), pid = rd32(a + 8);
      struct surface* s = surface_of(sid);
      struct surface* parent = surface_of(pid);
      if (!s || !parent) { post_error(id, 0, "get_subsurface on an unknown surface"); return; }
      // A surface may hold only one role at a time; the subsurface role is the one that matters
      // for compositing. Record the parent + a fresh z-order slot (creation order, adjusted by
      // place_above/place_below) so recomposite() can draw the layer.
      s->is_subsurface = 1;
      s->subsurface_parent = pid;
      s->subs_x = s->subs_y = 0;
      s->subs_z = g_substack_next++;
      obj_set(nid, IF_SUBSURFACE, o->version, (uint32_t)(s - g_surf));
      wlog("get_subsurface %u (surface %u) on parent %u", nid, sid, pid);
      return;
    }
    return;

  // -------------------------------------------------------------- wl_subsurface
  case IF_SUBSURFACE:
    if (o->data >= MAX_SURFACE) return;
    {
      struct surface* s = &g_surf[o->data];
      if (op == 0) {                                        // destroy
        if (s->is_subsurface) {
          s->is_subsurface = 0; s->subsurface_parent = 0;
          recomposite();                                    // its pixels must stop being drawn
        }
        delete_id(id); obj_del(id);
        return;
      }
      if (op == 1) {                                        // set_position(x, y)
        if (len >= 8) { s->subs_x = (int32_t)rd32(a); s->subs_y = (int32_t)rd32(a + 4); }
        recomposite();                                      // the layer may have moved
        return;
      }
      if (op == 2 && len >= 4) {                            // place_above(sibling)
        struct object* so = obj_get(rd32(a));
        if (so && so->iface == IF_SUBSURFACE && so->data < MAX_SURFACE)
          place_subsurface_above(s, &g_surf[so->data]);
        recomposite();
        return;
      }
      if (op == 3 && len >= 4) {                            // place_below(sibling)
        struct object* so = obj_get(rd32(a));
        if (so && so->iface == IF_SUBSURFACE && so->data < MAX_SURFACE)
          place_subsurface_below(s, &g_surf[so->data]);
        recomposite();
        return;
      }
      if (op == 4 || op == 5) return;                       // set_sync / set_desync: no-op here
      return;
    }

  case IF_CALLBACK:
  default:
    return;
  }
}

// ---------------------------------------------------------------------------
// connection loop
// ---------------------------------------------------------------------------
static void drain_requests(void) {
  size_t off = 0;
  while (g_in_n - off >= 8) {
    const uint8_t* m = g_in + off;
    uint32_t id = rd32(m);
    uint32_t sz_op = rd32(m + 4);
    uint16_t op = (uint16_t)(sz_op & 0xFFFF);
    uint16_t sz = (uint16_t)(sz_op >> 16);
    if (sz < 8) { wwarn("message size %u < 8, dropping connection", sz); g_in_n = 0; return; }
    if (g_in_n - off < sz) break;                            // partial: wait for more
    handle(id, op, m + 8, (size_t)sz - 8);
    off += sz;
  }
  if (off) { memmove(g_in, g_in + off, g_in_n - off); g_in_n -= off; }
}

static void client_readable(void) {
  union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int) * 16)]; } c;
  struct iovec iov = { g_in + g_in_n, sizeof g_in - g_in_n };
  if (iov.iov_len == 0) { wwarn("input buffer full"); g_in_n = 0; return; }
  struct msghdr mh; memset(&mh, 0, sizeof mh);
  mh.msg_iov = &iov; mh.msg_iovlen = 1;
  mh.msg_control = c.buf; mh.msg_controllen = sizeof c.buf;

  ssize_t r = recvmsg(g_cfd, &mh, 0);
  if (r == 0) { wlog("client disconnected"); close(g_cfd); g_cfd = -1; g_in_n = 0; return; }
  if (r < 0) {
    if (errno == EINTR || errno == EAGAIN) return;
    wwarn("recvmsg: %s", strerror(errno));
    close(g_cfd); g_cfd = -1; return;
  }
  for (struct cmsghdr* cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
    if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
    int n = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
    for (int i = 0; i < n; i++) {
      int fd; memcpy(&fd, CMSG_DATA(cm) + i * sizeof(int), sizeof fd);
      if (g_nrfd < (int)(sizeof g_rfd / sizeof g_rfd[0])) g_rfd[g_nrfd++] = fd;
      else { wwarn("fd queue overflow"); close(fd); }
    }
  }
  g_in_n += (size_t)r;
  drain_requests();
}

static void* wl_thread(void* arg) {
  (void)arg;
  while (!g_stop) {
    struct pollfd p[3];
    int n = 0;
    p[n].fd = g_lfd; p[n].events = POLLIN; p[n].revents = 0; n++;
    int clientIndex=-1;
    if (g_cfd >= 0) { clientIndex=n;p[n].fd = g_cfd; p[n].events = POLLIN; p[n].revents = 0; n++; }
    int feedbackIndex=-1;
    if (getenv("RDNATIVE_PRESENT_FEEDBACK") && rd_feedback_fd()>=0) {
      feedbackIndex=n;p[n].fd=rd_feedback_fd();p[n].events=POLLIN;p[n].revents=0;n++;
    }
    int r = poll(p, (nfds_t)n, 16);          // ~60 Hz: bounds input latency
    if (r < 0) { if (errno == EINTR) continue; break; }
    if (g_cfd >= 0) pump_input();
    if (p[0].revents & POLLIN) {
      int c = accept(g_lfd, NULL, NULL);
      if (c >= 0) {
        if (g_cfd >= 0) { close(c); }                        // one client is enough
        else {
          g_cfd = c;
          memset(g_obj, 0, sizeof g_obj);
          memset(g_server_obj, 0, sizeof g_server_obj);
          memset(g_sources, 0, sizeof g_sources);
          memset(g_dmabuf_params, 0, sizeof g_dmabuf_params);
          g_ndevices = 0; g_selection_source = 0; g_offer_next = SERVER_ID_LO;
          obj_set(1, IF_DISPLAY, 1, 0);                      // wl_display is always object 1
          g_in_n = 0; g_nrfd = 0;
          wlog("client connected (fd %d)", c);
        }
      }
    }
    if (clientIndex>=0 && (p[clientIndex].revents & (POLLIN | POLLHUP))) client_readable();
    if(feedbackIndex>=0 && (p[feedbackIndex].revents & POLLIN))rd_feedback_drain();
    if(feedbackIndex>=0)pump_present_feedback();
    if (g_cfd >= 0) pump_direct_frame_callbacks();
  }
  return NULL;
}

static void mac_surface_start(void);   // host-side Wayland client (Hyprland-owned mac surface)

// Host-side (macOS) mac-app helpers, in mac_surface_host.m. The ObjC/NSWorkspace
// launcher + best-effort window capture live there so wlsrv.c stays C.
extern int  rd_mac_launch_bundle(const char* spec, char* out_name, size_t onsz);
extern void* rd_mac_capture_window(pid_t pid, int* out_w, int* out_h, int* out_stride);

int wlsrv_start(const char* socket_path) {
  if (getenv("RDNATIVE_PRESENT_FEEDBACK") && rd_feedback_init()) return -1;
  // The compositor registers the cross-process dma-buf->IOSurface mach-port
  // receive channel before any client (shim_cli) can publish, so the exporter's
  // bootstrap_look_up() always finds it. Idempotent (pthread_once).
  rdgpu_handoff_import_init();
  snprintf(g_path, sizeof g_path, "%s", socket_path);
  unlink(g_path);
  { const char* st = getenv("WLSRV_SELFTEST");
    if (st && *st && *st != '0') wlsrv_convert_selftest(); }

  g_lfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (g_lfd < 0) { wwarn("socket: %s", strerror(errno)); return -1; }
  struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof sa.sun_path, "%s", g_path);
  if (bind(g_lfd, (struct sockaddr*)&sa, sizeof sa) < 0) {
    wwarn("bind %s: %s", g_path, strerror(errno)); close(g_lfd); g_lfd = -1; return -1;
  }
  if (listen(g_lfd, 4) < 0) {
    wwarn("listen: %s", strerror(errno)); close(g_lfd); g_lfd = -1; return -1;
  }
  if (pthread_create(&g_thread, NULL, wl_thread, NULL) != 0) {
    wwarn("pthread_create failed"); close(g_lfd); g_lfd = -1; return -1;
  }
  pthread_detach(g_thread);
  fprintf(stderr, "[wl] compositor listening on %s\n", g_path);
  mac_surface_start();
  return 0;
}

void wlsrv_stop(void) {
  g_stop = 1;
  if (g_cfd >= 0) { close(g_cfd); g_cfd = -1; }
  if (g_lfd >= 0) { close(g_lfd); g_lfd = -1; }
  unlink(g_path);
  if (getenv("RDNATIVE_PRESENT_FEEDBACK")) rd_feedback_close();
}

// Re-establish the compositor in a fork() CHILD, on a socket of the child's own.
//
// fork() keeps only the calling thread, so g_thread — the listener — does not exist in the child.
// What the child DOES inherit is g_lfd and g_cfd, descriptors onto the PARENT's socket, and the
// statics that describe a compositor which is no longer running. The result is a process holding
// a live-looking display connection that nothing will ever service: it blocks on its first round
// trip, forever, at 0% CPU. That is not a hypothetical — it is what a daemonizing guest (kate:
// dup3 /dev/null over 0/1/2, fork, setsid, fork) does on every run, and it reads exactly like
// "the app started and rendered nothing".
//
// Two things must NOT happen here:
//   * wlsrv_stop() must not be called. It unlink()s g_path, and g_path names the PARENT's socket.
//     Removing it would take the parent's display away as a side effect of the child starting.
//   * The child must not rebind the same path. wlsrv_start() unlinks before bind(), so a child
//     reusing the inherited name silently STEALS it from the parent — the failure c3e8a57 fixed
//     for concurrent shims, reintroduced through fork. The caller therefore passes a path in a
//     namespace keyed to the CHILD's pid.
//
// Per-client state is reset because the child has no client; the inherited shm mappings are left
// mapped rather than unmapped, since they belong to buffers the child will never look at again
// and unmapping them from under a half-copied table is the riskier of the two mistakes.
int wlsrv_restart_after_fork(const char* socket_path) {
  if (g_cfd >= 0) { close(g_cfd); g_cfd = -1; }     // the parent's client, not ours
  if (g_lfd >= 0) { close(g_lfd); g_lfd = -1; }     // the parent's listener, NOT unlinked
  g_stop = 0;
  g_in_n = 0;
  g_nrfd = 0;
  g_serial = 1;
  memset(g_obj,  0, sizeof g_obj);
  memset(g_pool, 0, sizeof g_pool);
  memset(g_buf,  0, sizeof g_buf);
  memset(g_dmabuf_params, 0, sizeof g_dmabuf_params);
  memset(g_surf, 0, sizeof g_surf);
  memset(g_pos,  0, sizeof g_pos);
  g_nptr = g_nkbd = 0;
  g_focus_surface = g_toplevel_surface = g_ptr_surface = 0;
  g_ptr_x = g_ptr_y = 0;
  g_interactive_serial = g_interactive_owner = 0;
  g_interactive_pressed = 0;
  g_ptr_host_window = 0;
  g_sent_x = g_sent_y = INT32_MIN;
  g_mods = 0;
  return wlsrv_start(socket_path);
}

// ===========================================================================
// mac-app window as a Hyprland-owned Wayland surface (host-side client)
// ===========================================================================
//
// The shim runs TWO compositors in one process:
//   * wlsrv.c (this file) is the OUTER compositor: Hyprland connects to it as a
//     Wayland CLIENT (aquamarine's wayland backend) and presents its whole desktop.
//   * Hyprland is the INNER compositor: foot/gtk/etc connect to ITS wayland-1 socket.
//
// A mac app window must be OWNED by Hyprland (moved/resized/focused like foot), so
// the shim must NOT synthesize another surface here in wlsrv — that would composite
// it in the OUTER layer, where Hyprland cannot manage it, and Hyprland would never
// see it as a window. The clean approach is for the shim to act as a Wayland CLIENT
// of Hyprland: it connects to Hyprland's own wayland-1 socket and creates a
// wl_surface + xdg_toplevel there. Hyprland then maps it (its log "Map request
// dispatched") and owns it exactly like a Linux client window.
//
// Both sockets are real host AF_UNIX sockets: net.c relocates the guest's
// /tmp/wayland-1 to <RDSOCKETGROUP>/tmp/wayland-1, and "a guest fd IS a host fd"
// (no fd translation table), so the shim can connect with a plain socket() and
// present a plain wl_shm buffer backed by a mkstemp+unlink anonymous file — no
// guest client binary is needed. The buffer content is a solid colour placeholder
// for this first stage; the mac app's Metal texture/IOSurface plugs into
// mac_surface_create_buffer() next.
//
// Trigger: RDNATIVE_MAC_APP_SURFACE=1 (optionally RDNATIVE_MAC_APP_W/H/TITLE/COLOR).

// object ids chosen by the shim. The client side assigns ids for every object it
// creates (wl_display is fixed at 1); Hyprland creates no objects for the four
// interfaces we bind, so low ids are safe and we never see a server id.
enum {
  // libwayland (as Hyprland 0.57 uses it) requires a client to allocate object ids
  // SEQUENTIALLY from 2, in creation order (matching libwayland-client). These are
  // ordered to match the exact creation sequence in mac_surface_handshake().
  MAC_O_REGISTRY    = 2,   // get_registry
  MAC_O_COMPOSITOR  = 3,   // bind wl_compositor
  MAC_O_SHM         = 4,   // bind wl_shm
  MAC_O_WM_BASE     = 5,   // bind xdg_wm_base
  MAC_O_SURFACE     = 6,   // create_surface
  MAC_O_POOL        = 7,   // wl_shm.create_pool
  MAC_O_BUFFER      = 8,   // wl_shm_pool.create_buffer
  MAC_O_XDG_SURFACE = 9,   // xdg_wm_base.get_xdg_surface
  MAC_O_TOPLEVEL    = 10,  // xdg_surface.get_toplevel
  MAC_O_BUFFER2     = 11,  // second wl_shm_pool.create_buffer (double buffering)
};

static int       mac_fd = -1;      // our connection to Hyprland's wayland-1 socket
static int       mac_w = 800, mac_h = 600;
static const char* mac_title = "Mac App";
static uint8_t   mac_r = 0x00, mac_g = 0xA8, mac_b = 0xE0;   // default teal (non-black)
static pthread_t mac_thread;
static int       mac_started = 0;

// Live-content state (Stage 2): a real mac app's pixels.
static IOSurfaceRef mac_iosurface = NULL;   // resolved app content (CFRetained), NULL = no content
static int       mac_carrier_fd = -1;       // carrier file fd for the IOSurface handoff
static char      mac_carrier_path[1100];    // carrier file path (handed to the test app)
static pid_t     mac_app_pid = 0;           // launched mac app pid (0 = none)
static char      mac_app_name[256];         // launched app display name (for the surface title)

// Persistent wl_shm pool + double buffer (two buffers from ONE pool, alternating,
// honouring wl_buffer.release before reusing a slot).
static int       mac_pool_fd = -1;
static uint8_t*  mac_pool_mem = NULL;
static size_t    mac_pool_size = 0;
static int       mac_stride = 0;
static int       mac_buf_created[2] = {0, 0};
static int       mac_buf_busy[2]    = {0, 0};

// Fallback content source: a captured window image (BGRA) for the NSWorkspace
// bundle path (e.g. Calculator). NULL when capture is unavailable (no TCC).
static uint8_t*  mac_capture_buf = NULL;
static int       mac_capture_w = 0, mac_capture_h = 0, mac_capture_stride = 0;
static uint64_t  mac_capture_attempts = 0;

// Send a buffered message (built with the shared ev_* helpers) to Hyprland.
static int mac_send(struct ev* e) {
  wr32(e->b + 4, (rd32(e->b + 4) & 0xFFFFu) | ((uint32_t)e->n << 16));
  size_t off = 0;
  while (off < e->n) {
    ssize_t w = send(mac_fd, e->b + off, e->n - off, 0);
    if (w < 0) { if (errno == EINTR) continue; return -1; }
    off += (size_t)w;
  }
  return 0;
}

// Same, but attach one fd via SCM_RIGHTS (wl_shm.create_pool).
static int mac_send_fd(struct ev* e, int fd) {
  wr32(e->b + 4, (rd32(e->b + 4) & 0xFFFFu) | ((uint32_t)e->n << 16));
  struct iovec iov = { e->b, e->n };
  union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } c;
  memset(&c, 0, sizeof c);
  struct msghdr mh; memset(&mh, 0, sizeof mh);
  mh.msg_iov = &iov; mh.msg_iovlen = 1;
  mh.msg_control = c.buf; mh.msg_controllen = CMSG_SPACE(sizeof(int));
  struct cmsghdr* cm = CMSG_FIRSTHDR(&mh);
  cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm), &fd, sizeof fd);
  ssize_t w = sendmsg(mac_fd, &mh, 0);
  return w < 0 ? -1 : 0;
}

// Decode a string argument at p; returns the pointer and advances *adv past it.
static const char* mac_str(const uint8_t* p, uint32_t* adv) {
  uint32_t l = rd32(p);
  uint32_t pad = (4 - (l & 3)) & 3;
  if (adv) *adv = 4 + l + pad;
  return (const char*)(p + 4);
}

// Read one whole Wayland message (blocking). Returns size (>=8) or -1 on EOF/error.
// We bind no fd-carrying interface, so inbound fds are impossible; recv is enough.
static ssize_t mac_read_msg(uint8_t* buf, size_t cap) {
  size_t got = 0;
  while (got < 8) {
    ssize_t r = recv(mac_fd, buf + got, 8 - got, 0);
    if (r == 0) return -1;
    if (r < 0) { if (errno == EINTR) continue; return -1; }
    got += (size_t)r;
  }
  uint16_t sz = (uint16_t)(rd32(buf + 4) >> 16);
  if (sz < 8 || sz > cap) return -1;
  while (got < sz) {
    ssize_t r = recv(mac_fd, buf + got, sz - got, 0);
    if (r == 0) return -1;
    if (r < 0) { if (errno == EINTR) continue; return -1; }
    got += (size_t)r;
  }
  return (ssize_t)sz;
}

static void mac_surface_path(char* out, size_t osz) {
  const char* g = getenv("RDSOCKETGROUP");
  if (!g || !*g) g = getenv("RDUNIXPREFIX");
  if (!g || !*g) g = "/tmp";
  snprintf(out, osz, "%s/tmp/wayland-1", g);
}

static int mac_surface_wait_socket(void) {
  char path[1100];
  mac_surface_path(path, sizeof path);
  for (int i = 0; i < 600 && !g_stop; i++) {     // 600 * 50ms = 30s
    struct stat st;
    if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
      fprintf(stderr, "[mac-surface] Hyprland wayland-1 socket ready: %s\n", path);
      return 0;
    }
    usleep(50000);
  }
  fprintf(stderr, "[mac-surface] timed out waiting for %s\n", path);
  return -1;
}

static int mac_surface_connect(void) {
  char path[1100];
  mac_surface_path(path, sizeof path);
  mac_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (mac_fd < 0) { fprintf(stderr, "[mac-surface] socket: %s\n", strerror(errno)); return -1; }
  int one = 1; setsockopt(mac_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
  sa.sun_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
  if (connect(mac_fd, (struct sockaddr*)&sa, (socklen_t)sa.sun_len) < 0) {
    fprintf(stderr, "[mac-surface] connect %s: %s\n", path, strerror(errno));
    close(mac_fd); mac_fd = -1; return -1;
  }
  fprintf(stderr, "[mac-surface] connected to Hyprland compositor\n");
  return 0;
}

// Fill one shm slot with content, in priority order:
//   1. the mac app's IOSurface (CPU readback — the "correctness first" path),
//   2. a captured window image (NSWorkspace bundle path, when TCC allows),
//   3. the solid placeholder colour.
// All sources are BGRA in memory, so rows map 1:1 onto ARGB8888 shm bytes.
static void mac_surface_fill_slot(int slot) {
  uint8_t* m = mac_pool_mem + (size_t)slot * (size_t)mac_stride * (size_t)mac_h;
  int filled = 0;
  if (mac_iosurface) {
    if (IOSurfaceLock(mac_iosurface, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess) {
      const uint8_t* base = (const uint8_t*)IOSurfaceGetBaseAddress(mac_iosurface);
      size_t pitch = IOSurfaceGetBytesPerRow(mac_iosurface);
      size_t sw = IOSurfaceGetWidth(mac_iosurface), sh = IOSurfaceGetHeight(mac_iosurface);
      size_t cw = (size_t)mac_w < sw ? (size_t)mac_w : sw;
      size_t ch = (size_t)mac_h < sh ? (size_t)mac_h : sh;
      if (base) {
        for (size_t y = 0; y < ch; y++)
          memcpy(m + y * (size_t)mac_stride, base + y * pitch, cw * 4);
        filled = 1;
      }
      IOSurfaceUnlock(mac_iosurface, kIOSurfaceLockReadOnly, NULL);
    }
  }
  if (!filled && mac_capture_buf) {
    // Top-left crop of the captured window (no scale; the app window is usually
    // smaller than the surface). Clear the remainder to opaque black.
    memset(m, 0, (size_t)mac_h * (size_t)mac_stride);
    for (int y = 0; y < mac_h; y++) {
      uint8_t* row = m + (size_t)y * mac_stride;
      for (int x = 0; x < mac_w; x++) row[x * 4 + 3] = 0xFF;
    }
    int cw = mac_capture_w < mac_w ? mac_capture_w : mac_w;
    int ch = mac_capture_h < mac_h ? mac_capture_h : mac_h;
    for (int y = 0; y < ch; y++)
      memcpy(m + (size_t)y * mac_stride,
             mac_capture_buf + (size_t)y * mac_capture_stride, (size_t)cw * 4);
    filled = 1;
  }
  if (!filled) {
    for (int y = 0; y < mac_h; y++) {             // ARGB8888 little-endian -> B,G,R,A in memory
      uint8_t* row = m + (size_t)y * mac_stride;
      for (int x = 0; x < mac_w; x++) { row[x*4+0] = mac_b; row[x*4+1] = mac_g; row[x*4+2] = mac_r; row[x*4+3] = 0xFF; }
    }
  }
}

// Create the persistent wl_shm pool (two buffers' worth in ONE anonymous file),
// map it, fill both slots, and send wl_shm.create_pool. The fd stays open and the
// mapping stays live for the pool's lifetime (so we can re-commit live content).
static int mac_surface_create_pool(void) {
  mac_stride = mac_w * 4;
  mac_pool_size = (size_t)mac_h * (size_t)mac_stride * 2;   // double buffer
  char tmpl[] = "/tmp/rdmac-XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) { fprintf(stderr, "[mac-surface] mkstemp: %s\n", strerror(errno)); return -1; }
  if (ftruncate(fd, (off_t)mac_pool_size) < 0) { fprintf(stderr, "[mac-surface] ftruncate: %s\n", strerror(errno)); close(fd); return -1; }
  unlink(tmpl);                                             // anonymous: lives only via the fd
  uint8_t* m = mmap(NULL, mac_pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) { fprintf(stderr, "[mac-surface] mmap: %s\n", strerror(errno)); close(fd); return -1; }
  mac_pool_fd = fd;
  mac_pool_mem = m;

  struct ev e;
  ev_begin(&e, MAC_O_SHM, 0);                               // wl_shm.create_pool(new_id, fd, size)
  ev_u32(&e, MAC_O_POOL);
  ev_u32(&e, (uint32_t)mac_pool_size);
  if (mac_send_fd(&e, fd) < 0) { fprintf(stderr, "[mac-surface] create_pool send failed\n"); return -1; }

  mac_surface_fill_slot(0);
  mac_surface_fill_slot(1);

  // Create buffer 0 EAGERLY (id 8) so object ids stay sequential with no gap:
  // Hyprland rejects a later get_xdg_surface(new_id=9) as "not a valid new object
  // id" when id 8 was never allocated. Buffer 2 (id 11) is created lazily later.
  ev_begin(&e, MAC_O_POOL, 0);                              // wl_shm_pool.create_buffer(...)
  ev_u32(&e, MAC_O_BUFFER);
  ev_u32(&e, 0);                                            // offset 0
  ev_u32(&e, (uint32_t)mac_w);
  ev_u32(&e, (uint32_t)mac_h);
  ev_u32(&e, (uint32_t)mac_stride);
  ev_u32(&e, 0);                                            // WL_SHM_FORMAT_ARGB8888
  mac_send(&e);
  mac_buf_created[0] = 1;

  fprintf(stderr, "[mac-surface] shm pool %dx%d x2 stride %d%s\n",
          mac_w, mac_h, mac_stride, mac_iosurface ? " (content: IOSurface readback)" : " (solid placeholder)");
  return 0;
}

// Fill a slot, (lazily) create its wl_buffer, attach it, damage the whole surface,
// and commit. Used for the initial map and for every subsequent live frame.
static int mac_surface_commit_slot(int slot) {
  if (slot < 0 || slot > 1) return -1;
  mac_surface_fill_slot(slot);
  uint32_t bufid = (slot == 0) ? MAC_O_BUFFER : MAC_O_BUFFER2;
  if (!mac_buf_created[slot]) {
    struct ev e;
    ev_begin(&e, MAC_O_POOL, 0);                            // wl_shm_pool.create_buffer(...)
    ev_u32(&e, bufid);
    ev_u32(&e, (uint32_t)((size_t)slot * (size_t)mac_stride * (size_t)mac_h));
    ev_u32(&e, (uint32_t)mac_w);
    ev_u32(&e, (uint32_t)mac_h);
    ev_u32(&e, (uint32_t)mac_stride);
    ev_u32(&e, 0);                                          // WL_SHM_FORMAT_ARGB8888
    mac_send(&e);
    mac_buf_created[slot] = 1;
  }
  struct ev e;
  ev_begin(&e, MAC_O_SURFACE, 1); ev_u32(&e, bufid); ev_u32(&e, 0); ev_u32(&e, 0); // attach
  mac_send(&e);
  ev_begin(&e, MAC_O_SURFACE, 2);                           // damage(0,0,w,h)
  ev_u32(&e, 0); ev_u32(&e, 0); ev_u32(&e, (uint32_t)mac_w); ev_u32(&e, (uint32_t)mac_h);
  mac_send(&e);
  ev_begin(&e, MAC_O_SURFACE, 6);                           // commit
  mac_send(&e);
  mac_buf_busy[slot] = 1;
  return 0;
}

// Commit a new frame on the first free slot; -1 if both slots are still held by
// the compositor (caller retries after the next wl_buffer.release).
static int mac_surface_commit_frame(void) {
  int slot = !mac_buf_busy[0] ? 0 : (!mac_buf_busy[1] ? 1 : -1);
  if (slot < 0) return -1;
  return mac_surface_commit_slot(slot);
}

// One-shot debug dump of the ACTUAL wl_shm bytes (slot 0), proving the surface
// carries the app's pixels rather than the placeholder. RDNATIVE_MAC_APP_DUMP=<path>
static void mac_surface_dump_once(void) {
  static int done;
  if (done) return;
  const char* p = getenv("RDNATIVE_MAC_APP_DUMP");
  if (!p || !*p) { done = 1; return; }
  done = 1;
  if (!mac_pool_mem) return;
  FILE* f = fopen(p, "wb");
  if (!f) return;
  const uint8_t* m = mac_pool_mem;
  fprintf(f, "P6\n%d %d\n255\n", mac_w, mac_h);
  for (int y = 0; y < mac_h; y++) {
    const uint8_t* row = m + (size_t)y * mac_stride;
    for (int x = 0; x < mac_w; x++) { fputc(row[x*4+2], f); fputc(row[x*4+1], f); fputc(row[x*4+0], f); }
  }
  fclose(f);
  fprintf(stderr, "[mac-surface] dumped wl_shm content -> %s (%dx%d)\n", p, mac_w, mac_h);
}

static void mac_bind(uint32_t reg, uint32_t name, const char* iface, uint32_t ver, uint32_t new_id) {
  struct ev e;
  ev_begin(&e, reg, 0);                           // wl_registry.bind
  ev_u32(&e, name);
  ev_str(&e, iface);
  ev_u32(&e, ver);
  ev_u32(&e, new_id);
  mac_send(&e);
}

// Connect, discover globals, bind, create the surface + role + buffer, then issue
// the first (empty) commit that makes Hyprland send its initial configure.
static int mac_surface_handshake(void) {
  struct ev e;
  ev_begin(&e, 1, 1); ev_u32(&e, MAC_O_REGISTRY); // wl_display.get_registry(new_id)
  if (mac_send(&e) < 0) return -1;

  uint32_t nc = 0, ns = 0, nw = 0;
  int have = 0;
  uint8_t buf[8192];
  for (int it = 0; it < 512 && have < 3 && !g_stop; it++) {
    ssize_t n = mac_read_msg(buf, sizeof buf);
    if (n <= 0) return -1;
    uint32_t id = rd32(buf);
    uint16_t op = (uint16_t)(rd32(buf + 4) & 0xFFFF);
    if (id == MAC_O_REGISTRY && op == 0) {        // wl_registry.global(name, iface, version)
      uint32_t nm = rd32(buf + 8);
      uint32_t adv = 0; const char* iface = mac_str(buf + 12, &adv);
      if (!iface) continue;
      if (!strcmp(iface, "wl_compositor")) { nc = nm; have++; }
      else if (!strcmp(iface, "wl_shm")) { ns = nm; have++; }
      else if (!strcmp(iface, "xdg_wm_base")) { nw = nm; have++; }
    }
  }
  if (have < 3) { fprintf(stderr, "[mac-surface] missing globals (%d/3)\n", have); return -1; }
  fprintf(stderr, "[mac-surface] globals wl_compositor=%u wl_shm=%u xdg_wm_base=%u\n", nc, ns, nw);

  mac_bind(MAC_O_REGISTRY, nc, "wl_compositor", 4, MAC_O_COMPOSITOR);
  mac_bind(MAC_O_REGISTRY, ns, "wl_shm", 1, MAC_O_SHM);
  mac_bind(MAC_O_REGISTRY, nw, "xdg_wm_base", 3, MAC_O_WM_BASE);

  ev_begin(&e, MAC_O_COMPOSITOR, 0); ev_u32(&e, MAC_O_SURFACE);   // create_surface
  mac_send(&e);
  if (mac_surface_create_pool() < 0) return -1;

  ev_begin(&e, MAC_O_WM_BASE, 2);                 // get_xdg_surface(new_id, surface)
  ev_u32(&e, MAC_O_XDG_SURFACE); ev_u32(&e, MAC_O_SURFACE);
  mac_send(&e);
  ev_begin(&e, MAC_O_XDG_SURFACE, 1); ev_u32(&e, MAC_O_TOPLEVEL); // get_toplevel(new_id)
  mac_send(&e);
  ev_begin(&e, MAC_O_TOPLEVEL, 2); ev_str(&e, mac_title);          // set_title
  mac_send(&e);
  ev_begin(&e, MAC_O_TOPLEVEL, 3); ev_str(&e, "rd.mac.app");       // set_app_id
  mac_send(&e);

  ev_begin(&e, MAC_O_SURFACE, 6);                 // commit -> triggers initial configure
  mac_send(&e);
  fprintf(stderr, "[mac-surface] surface %u + xdg_toplevel %u created, title '%s'\n",
          MAC_O_SURFACE, MAC_O_TOPLEVEL, mac_title);
  return 0;
}

static void mac_surface_dispatch(uint32_t id, uint16_t op, const uint8_t* a, size_t len, int* mapped) {
  struct ev e;
  if (id == MAC_O_WM_BASE && op == 0) {           // xdg_wm_base.ping(serial)
    uint32_t serial = len >= 4 ? rd32(a) : 0;
    ev_begin(&e, MAC_O_WM_BASE, 3); ev_u32(&e, serial);            // pong
    mac_send(&e);
  } else if (id == MAC_O_XDG_SURFACE && op == 0) { // xdg_surface.configure(serial)
    uint32_t serial = len >= 4 ? rd32(a) : 0;
    ev_begin(&e, MAC_O_XDG_SURFACE, 4); ev_u32(&e, serial);        // ack_configure
    mac_send(&e);
    if (!*mapped) {
      mac_surface_commit_slot(0);                  // attach(buffer0)+damage+commit -> maps
      *mapped = 1;
      mac_surface_dump_once();
      fprintf(stderr, "[mac-surface] acked configure %u, attached buffer + committed; window maps\n", serial);
    }
  } else if (id == MAC_O_TOPLEVEL && op == 0) {   // xdg_toplevel.configure(w, h, states)
    int32_t w = len >= 4 ? (int32_t)rd32(a) : 0;
    int32_t h = len >= 8 ? (int32_t)rd32(a + 4) : 0;
    fprintf(stderr, "[mac-surface] xdg_toplevel.configure %dx%d\n", w, h);
  } else if (id == MAC_O_TOPLEVEL && op == 1) {   // xdg_toplevel.close
    fprintf(stderr, "[mac-surface] xdg_toplevel.close (placeholder: ignored)\n");
  } else if (id == MAC_O_BUFFER && op == 0) {     // wl_buffer.release (slot 0 free again)
    mac_buf_busy[0] = 0;
  } else if (id == MAC_O_BUFFER2 && op == 0) {    // wl_buffer.release (slot 1 free again)
    mac_buf_busy[1] = 0;
  } else if (id == 1 && op == 0) {                // wl_display.error(object, code, message)
    uint32_t obj  = len >= 4  ? rd32(a)     : 0;
    uint32_t code = len >= 8  ? rd32(a + 4) : 0;
    uint32_t adv = 0; const char* msg = len >= 12 ? mac_str(a + 8, &adv) : "";
    fprintf(stderr, "[mac-surface] wl_display.error object=%u code=%u msg='%s'\n", obj, code, msg);
  }
}

// Hyprland's wayland-1 socket appears mid-init, BEFORE its EGL/DRM setup finishes
// opening descriptors. Because "a guest fd IS a host fd" (one shared fd table),
// connecting here would steal an fd number Hyprland's EGL init is about to use, and
// EGL would then query the wrong descriptor ("pci id ... driver (null)" → "egl: failed
// to create dri2 screen"). Wait until Hyprland has presented its first frame to the
// OUTER compositor (g_frames>0): by then its fd table is stable and connecting is safe.
static int mac_surface_wait_ready(void) {
  for (int i = 0; i < 300 && !g_stop; i++) {     // 300 * 100ms = 30s
    if (g_frames > 0) {
      fprintf(stderr, "[mac-surface] Hyprland rendered frame %llu; connecting\n",
              (unsigned long long)g_frames);
      return 0;
    }
    usleep(100000);
  }
  fprintf(stderr, "[mac-surface] timed out waiting for Hyprland's first frame\n");
  return -1;
}

// Create the carrier file whose fstat identity keys the cross-process IOSurface
// handoff (see rdgpu_surface_handoff.c). Both the shim and the launched test app
// open the SAME path, so they derive the same (st_dev, st_ino).
static int mac_create_carrier(char* out, size_t osz) {
  const char* g = getenv("RDSOCKETGROUP");
  char tmpl[1100];
  if (g && *g) snprintf(tmpl, sizeof tmpl, "%s/mac-carrier-XXXXXX", g);
  else snprintf(tmpl, sizeof tmpl, "/tmp/rdmac-carrier-XXXXXX");
  int fd = mkstemp(tmpl);
  if (fd < 0) { fprintf(stderr, "[mac-surface] mkstemp carrier: %s\n", strerror(errno)); return -1; }
  snprintf(out, osz, "%s", tmpl);
  return fd;
}

// Launch the IOSurface test app (rd_mac_app) as a child, inheriting $RDSOCKETGROUP
// so both sides derive the same handoff bootstrap name. The carrier path (and the
// surface size) travel as argv.
static pid_t mac_launch_test_app(const char* exe, const char* carrier) {
  pid_t p = fork();
  if (p < 0) { fprintf(stderr, "[mac-surface] fork: %s\n", strerror(errno)); return -1; }
  if (p == 0) {
    char car[1200], wb[32], hb[32];
    snprintf(car, sizeof car, "--rdmac-carrier=%s", carrier);
    snprintf(wb, sizeof wb, "--rdmac-w=%d", mac_w);
    snprintf(hb, sizeof hb, "--rdmac-h=%d", mac_h);
    execl(exe, exe, car, wb, hb, (char*)NULL);
    _exit(127);
  }
  return p;
}

// Resolve the carrier fd to the app's published IOSurface (a +1 reference we own),
// and adopt its real dimensions. Returns 0 on success, -1 if not yet available.
static int mac_surface_resolve_iosurface(void) {
  if (mac_carrier_fd < 0) return -1;
  if (mac_iosurface) return 0;
  IOSurfaceRef s = rdgpu_handoff_resolve_fd(mac_carrier_fd);
  if (!s) return -1;
  mac_iosurface = s;
  int w = (int)IOSurfaceGetWidth(s), h = (int)IOSurfaceGetHeight(s);
  if (w > 0) mac_w = w;
  if (h > 0) mac_h = h;
  fprintf(stderr, "[mac-surface] resolved mac-app IOSurface id=%u %dx%d stride=%zu\n",
          IOSurfaceGetID(s), mac_w, mac_h, IOSurfaceGetBytesPerRow(s));
  return 0;
}

static void* mac_surface_thread(void* arg) {
  (void)arg;
  if (mac_surface_wait_socket() < 0) return NULL;
  if (mac_surface_wait_ready() < 0) return NULL;
  if (mac_surface_connect() < 0) return NULL;
  // Resolve the mac app's IOSurface (when a test app was launched) BEFORE the
  // pool is sized, so mac_w/mac_h match the app's real surface. Bounded retry so
  // a slow app start falls back to placeholder rather than stranding the surface.
  if (mac_carrier_fd >= 0) {
    for (int i = 0; i < 12 && !mac_iosurface && !g_stop; i++) {
      if (mac_surface_resolve_iosurface() == 0) break;
      usleep(250000);
    }
    if (!mac_iosurface)
      fprintf(stderr, "[mac-surface] IOSurface not resolved; using solid placeholder\n");
  }
  if (mac_surface_handshake() < 0) { close(mac_fd); mac_fd = -1; return NULL; }

  int mapped = 0;
  uint8_t buf[8192];
  while (!g_stop) {
    int live = (mac_iosurface != NULL || mac_capture_buf != NULL);
    struct pollfd pfd = { mac_fd, POLLIN, 0 };
    int r = poll(&pfd, 1, live ? 66 : 500);    // ~15fps when live, idle otherwise
    if (r < 0) { if (errno == EINTR) continue; break; }
    if (pfd.revents & (POLLHUP | POLLERR)) { fprintf(stderr, "[mac-surface] Hyprland closed the connection\n"); break; }
    if (r == 0) {
      if (mapped) {
        // No IOSurface but a launched bundle app: try to capture its window
        // (throttled ~1/2s; needs Screen Recording TCC for foreign apps).
        if (!mac_iosurface && mac_app_pid > 0 &&
            (mac_capture_attempts++ & 3) == 0) {
          int cw = 0, ch = 0, cs = 0;
          uint8_t* cbuf = rd_mac_capture_window(mac_app_pid, &cw, &ch, &cs);
          if (cbuf) { free(mac_capture_buf); mac_capture_buf = cbuf; mac_capture_w = cw; mac_capture_h = ch; mac_capture_stride = cs; }
        }
        if (mac_iosurface || mac_capture_buf) mac_surface_commit_frame();
      }
      continue;
    }
    ssize_t n = mac_read_msg(buf, sizeof buf);
    if (n <= 0) break;
    uint32_t id = rd32(buf);
    uint16_t op = (uint16_t)(rd32(buf + 4) & 0xFFFF);
    mac_surface_dispatch(id, op, buf + 8, (size_t)n - 8, &mapped);
  }
  close(mac_fd); mac_fd = -1;
  return NULL;
}

static void mac_surface_start(void) {
  const char* v = getenv("RDNATIVE_MAC_APP_SURFACE");
  if (!v || strcmp(v, "1")) return;
  if (mac_started) return;                         // also guards fork children (flag is copied)
  mac_started = 1;

  const char* w = getenv("RDNATIVE_MAC_APP_W"); if (w && atoi(w) > 0) mac_w = atoi(w);
  const char* h = getenv("RDNATIVE_MAC_APP_H"); if (h && atoi(h) > 0) mac_h = atoi(h);
  const char* t = getenv("RDNATIVE_MAC_APP_TITLE"); if (t && *t) mac_title = t;
  const char* c = getenv("RDNATIVE_MAC_APP_COLOR");
  if (c && strlen(c) == 6) {
    unsigned vcol = (unsigned)strtol(c, NULL, 16);
    mac_r = (uint8_t)((vcol >> 16) & 0xFF); mac_g = (uint8_t)((vcol >> 8) & 0xFF); mac_b = (uint8_t)(vcol & 0xFF);
  }

  // Task 2 launcher: RDNATIVE_MAC_APP_BUNDLE=<bundle-id|name|path> launches a real
  // mac app via NSWorkspace and titles the Hyprland-owned surface after it.
  const char* bundle = getenv("RDNATIVE_MAC_APP_BUNDLE");
  if (bundle && *bundle) {
    char name[256] = {0};
    pid_t pid = rd_mac_launch_bundle(bundle, name, sizeof name);
    if (pid > 0) {
      mac_app_pid = pid;
      snprintf(mac_app_name, sizeof mac_app_name, "%s", name[0] ? name : bundle);
      mac_title = mac_app_name;
    }
  }

  // Task 1 content path: RDNATIVE_MAC_APP_TEST=<test-app-binary> launches the tiny
  // Metal/CAMetalLayer app (rd_mac_app) and wires its shared IOSurface into the
  // wl_shm buffer — the "real app pixels" proof that needs no TCC and no sudo.
  const char* testexe = getenv("RDNATIVE_MAC_APP_TEST");
  if (testexe && *testexe) {
    mac_carrier_fd = mac_create_carrier(mac_carrier_path, sizeof mac_carrier_path);
    if (mac_carrier_fd >= 0) {
      pid_t p = mac_launch_test_app(testexe, mac_carrier_path);
      if (p > 0) {
        mac_app_pid = p;
        if (!mac_app_name[0]) snprintf(mac_app_name, sizeof mac_app_name, "rd-mac-app");
        if (!bundle || !*bundle) mac_title = mac_app_name;   // bundle name wins when present
      } else {
        close(mac_carrier_fd); mac_carrier_fd = -1;
      }
    }
  }

  fprintf(stderr, "[mac-surface] host trigger: creating Hyprland-owned surface %dx%d title='%s' color=#%02X%02X%02X%s\n",
          mac_w, mac_h, mac_title, mac_r, mac_g, mac_b,
          mac_carrier_fd >= 0 ? " content=IOSurface" : " content=placeholder");
  if (pthread_create(&mac_thread, NULL, mac_surface_thread, NULL) != 0)
    fprintf(stderr, "[mac-surface] pthread_create failed\n");
  else
    pthread_detach(mac_thread);
}
