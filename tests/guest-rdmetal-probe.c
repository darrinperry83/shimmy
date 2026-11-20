// guest-rdmetal-probe.c — exercise the virtual DRM node end to end with libdrm.
//
// Build against an Arch aarch64 root (headers + libdrm) and run under shim_cli.
// On success it emits RDMETAL_PROBE_OK; every step that fails reports which
// ioctl/query failed and exits non-zero so the harness can bisect the gap.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define FAIL(...) do { fprintf(stderr, "RDMETAL_PROBE_FAIL "); fprintf(stderr, __VA_ARGS__); \
                       fprintf(stderr, "\n"); return __LINE__; } while (0)

static int g_fd = -1;

static int dump_caps(void) {
  static const struct { uint64_t cap; const char* name; } caps[] = {
    { DRM_CAP_DUMB_BUFFER,         "DUMB_BUFFER" },
    { DRM_CAP_PRIME,               "PRIME" },
    { DRM_CAP_TIMESTAMP_MONOTONIC, "TIMESTAMP_MONOTONIC" },
    { DRM_CAP_ASYNC_PAGE_FLIP,     "ASYNC_PAGE_FLIP" },
    { DRM_CAP_CURSOR_WIDTH,        "CURSOR_WIDTH" },
    { DRM_CAP_CURSOR_HEIGHT,       "CURSOR_HEIGHT" },
    { DRM_CAP_ADDFB2_MODIFIERS,    "ADDFB2_MODIFIERS" },
    { DRM_CAP_CRTC_IN_VBLANK_EVENT,"CRTC_IN_VBLANK_EVENT" },
    { DRM_CAP_SYNCOBJ,             "SYNCOBJ" },
    { DRM_CAP_SYNCOBJ_TIMELINE,    "SYNCOBJ_TIMELINE" },
  };
  for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++) {
    uint64_t value = 0;
    int r = drmGetCap(g_fd, caps[i].cap, &value);
    printf("cap %-24s -> r=%d value=%llu\n", caps[i].name, r,
           (unsigned long long)value);
  }
  // DUMB_BUFFER and PRIME are the two aquamarine hard-requires.
  uint64_t v = 0;
  if (drmGetCap(g_fd, DRM_CAP_DUMB_BUFFER, &v) || v != 1)
    FAIL("DRM_CAP_DUMB_BUFFER not 1 (value=%llu)", (unsigned long long)v);
  if (drmGetCap(g_fd, DRM_CAP_PRIME, &v) || !(v & DRM_PRIME_CAP_IMPORT))
    FAIL("DRM_CAP_PRIME missing IMPORT (value=%llu)", (unsigned long long)v);
  return 0;
}

int main(void) {
  g_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (g_fd < 0) FAIL("open /dev/dri/card0: %s", strerror(errno));
  printf("opened /dev/dri/card0 fd=%d\n", g_fd);

  int render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
  if (render < 0) FAIL("open /dev/dri/renderD128: %s", strerror(errno));
  printf("opened /dev/dri/renderD128 fd=%d\n", render);

  // glibc's realpath() treats readlink()==EINVAL on a non-symlink as "ordinary
  // component"; ENOENT makes kwin's QFileInfo::canonicalFilePath() empty and
  // open("") fail. Guard the exact regression that used to block KMS startup.
  char canon[PATH_MAX];
  if (!realpath("/dev/dri/card0", canon) || strcmp(canon, "/dev/dri/card0") != 0)
    FAIL("realpath(/dev/dri/card0) -> %s", canon[0] ? canon : "(null)");
  printf("realpath /dev/dri/card0 -> %s\n", canon);

  dump_caps();

  drmModeRes *res = drmModeGetResources(g_fd);
  if (!res) FAIL("drmModeGetResources: %s", strerror(errno));
  printf("resources crtcs=%u connectors=%u encoders=%u fbs=%u\n",
         res->count_crtcs, res->count_connectors, res->count_encoders, res->count_fbs);
  if (res->count_crtcs < 1 || res->count_connectors < 1 || res->count_encoders < 1)
    FAIL("GETRESOURCES returned empty object lists");

  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *c = drmModeGetConnector(g_fd, res->connectors[i]);
    if (!c) FAIL("drmModeGetConnector(%u): %s", res->connectors[i], strerror(errno));
    printf("connector %u type=%u connection=%u modes=%u encoders=%u props=%u\n",
           c->connector_id, c->connector_type, c->connection,
           c->count_modes, c->count_encoders, c->count_props);
    drmModeFreeConnector(c);
  }
  for (int i = 0; i < res->count_encoders; i++) {
    drmModeEncoder *e = drmModeGetEncoder(g_fd, res->encoders[i]);
    if (!e) FAIL("drmModeGetEncoder(%u): %s", res->encoders[i], strerror(errno));
    printf("encoder %u type=%u crtc=%u possible=%u\n", e->encoder_id, e->encoder_type,
           e->crtc_id, e->possible_crtcs);
    drmModeFreeEncoder(e);
  }
  for (int i = 0; i < res->count_crtcs; i++) {
    drmModeCrtc *cr = drmModeGetCrtc(g_fd, res->crtcs[i]);
    if (!cr) FAIL("drmModeGetCrtc(%u): %s", res->crtcs[i], strerror(errno));
    printf("crtc %u mode_valid=%u fb_id=%u\n", cr->crtc_id, cr->mode_valid, cr->buffer_id);
    drmModeFreeCrtc(cr);
  }

  drmModePlaneRes *pres = drmModeGetPlaneResources(g_fd);
  if (!pres) FAIL("drmModeGetPlaneResources: %s", strerror(errno));
  printf("planes count=%u\n", pres->count_planes);
  if (pres->count_planes < 1) FAIL("GETPLANERESOURCES returned no planes");
  for (uint32_t i = 0; i < pres->count_planes; i++) {
    drmModePlane *pl = drmModeGetPlane(g_fd, pres->planes[i]);
    if (!pl) FAIL("drmModeGetPlane(%u): %s", pres->planes[i], strerror(errno));
    printf("plane %u crtc=%u fb=%u formats=%u possible=%u\n", pl->plane_id,
           pl->crtc_id, pl->fb_id, pl->count_formats, pl->possible_crtcs);
    drmModeFreePlane(pl);
  }
  drmModeFreePlaneResources(pres);

  // Object + property model (aquamarine resolves atomic props by name).
  for (uint32_t i = 0; i < (uint32_t)res->count_crtcs; i++) {
    drmModeObjectProperties *op =
        drmModeObjectGetProperties(g_fd, res->crtcs[i], DRM_MODE_OBJECT_CRTC);
    if (!op) FAIL("drmModeObjectGetProperties(crtc %u): %s", res->crtcs[i], strerror(errno));
    printf("crtc %u props=%u\n", res->crtcs[i], op->count_props);
    for (uint32_t j = 0; j < op->count_props; j++) {
      drmModePropertyRes *p = drmModeGetProperty(g_fd, op->props[j]);
      if (!p) FAIL("drmModeGetProperty(%u): %s", op->props[j], strerror(errno));
      drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(op);
  }

  // Dumb buffer create + map + mmap + ADDFB2 + PRIME export.
  uint32_t handle = 0, pitch = 0;
  uint64_t size = 0;
  if (drmModeCreateDumbBuffer(g_fd, 64, 64, 32, 0, &handle, &pitch, &size))
    FAIL("drmModeCreateDumbBuffer: %s", strerror(errno));
  printf("dumb handle=0x%x pitch=%u size=%llu\n", handle, pitch, (unsigned long long)size);
  if (!handle || !pitch || !size) FAIL("dumb buffer has zero handle/pitch/size");

  uint64_t offset = 0;
  if (drmModeMapDumbBuffer(g_fd, handle, &offset))
    FAIL("drmModeMapDumbBuffer: %s", strerror(errno));
  printf("dumb map offset=0x%llx\n", (unsigned long long)offset);

  void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, offset);
  if (map == MAP_FAILED) FAIL("mmap dumb buffer: %s", strerror(errno));
  memset(map, 0x7f, 64 * 64 * 4);
  printf("mmap + memset ok\n");

  uint32_t fb_id = 0;
  uint32_t fb_handles[4] = { handle, 0, 0, 0 };
  uint32_t fb_pitches[4] = { pitch, 0, 0, 0 };
  uint32_t fb_offsets[4] = { 0, 0, 0, 0 };
  uint64_t modifiers[4] = { 0, 0, 0, 0 };
  if (drmModeAddFB2WithModifiers(g_fd, 64, 64, DRM_FORMAT_XRGB8888,
                                 fb_handles, fb_pitches, fb_offsets, modifiers,
                                 &fb_id, DRM_MODE_FB_MODIFIERS))
    FAIL("drmModeAddFB2WithModifiers: %s", strerror(errno));
  printf("addfb2 fb_id=%u\n", fb_id);

  int prime_fd = -1;
  if (drmPrimeHandleToFD(g_fd, handle, DRM_CLOEXEC, &prime_fd))
    FAIL("drmPrimeHandleToFD: %s", strerror(errno));
  printf("prime handle 0x%x -> fd=%d\n", handle, prime_fd);
  if (prime_fd < 0) FAIL("drmPrimeHandleToFD returned bad fd");
  close(prime_fd);

  if (drmModeRmFB(g_fd, fb_id)) FAIL("drmModeRmFB: %s", strerror(errno));
  munmap(map, size);
  if (drmModeDestroyDumbBuffer(g_fd, handle))
    FAIL("drmModeDestroyDumbBuffer: %s", strerror(errno));
  printf("fb/dumb cleanup ok\n");

  drmModeFreeResources(res);
  close(render);
  close(g_fd);
  printf("RDMETAL_PROBE_OK\n");
  return 0;
}
