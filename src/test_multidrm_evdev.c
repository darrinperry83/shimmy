/* test_multidrm_evdev.c — host-side validation of the per-CRTC present + evdev input path.
 *
 * Part 1 (DRM per-CRTC present): create two dumb BOs (3440x1440 and 3600x2338), wrap them in
 * framebuffers, and commit a single MODE_ATOMIC with TWO plane objects (plane 1 -> crtc 1,
 * plane 2 -> crtc 2). The shim's per-CRTC drm_present_fb must fire for BOTH: check stderr for
 * "DRM_PRESENT crtc=1 ... 3440x1440 non-black" and "DRM_PRESENT crtc=2 ... 3600x2338 non-black".
 *
 * Part 2 (evdev node): open /dev/input/event0, run EVIOCGNAME/EVIOCGID/EVIOCGBIT, inject a
 * key + SYN and read back the 24-byte input_event records.
 *
 * Link against drmasahi.c + evdev.c (+ rdgpu_surface_handoff.c for drmasahi's deps).
 */
#include "drmasahi.h"
#include "evdev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define DRM_IOCTL_MODE_CREATE_DUMB 0xc02064b2ULL
#define DRM_IOCTL_MODE_MAP_DUMB    0xc01064b3ULL
#define DRM_IOCTL_MODE_ADDFB2      0xc06864b8ULL
#define DRM_IOCTL_MODE_ATOMIC      0xc03864bcULL

struct drm_mode_create_dumb { uint32_t height, width, bpp, flags; uint32_t handle, pitch; uint64_t size; };
struct drm_mode_map_dumb    { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_fb_cmd2 {
  uint32_t fb_id, width, height, pixel_format, flags;
  uint32_t handles[4], pitches[4], offsets[4];
  uint64_t modifier[4];
};
struct drm_mode_atomic {
  uint32_t flags, count_objs;
  uint64_t objs_ptr, count_props_ptr, props_ptr, prop_values_ptr, reserved, user_data;
};

#define DRM_FORMAT_XRGB8888 0x34325258u   // 'XR24'
#define PROP_CRTC_ID 4
#define PROP_FB_ID   5

static uint32_t make_bo(uint32_t w, uint32_t h, uint32_t* out_pitch) {
  struct drm_mode_create_dumb c = { h, w, 32, 0, 0, 0, 0 };
  if (drm_ioctl(0, DRM_IOCTL_MODE_CREATE_DUMB, (unsigned long)&c) != 0 || !c.handle) {
    fprintf(stderr, "CREATE_DUMB %ux%u failed\n", w, h); exit(2);
  }
  struct drm_mode_map_dumb m = { c.handle, 0, 0 };
  if (drm_ioctl(0, DRM_IOCTL_MODE_MAP_DUMB, (unsigned long)&m) != 0 || !m.offset) {
    fprintf(stderr, "MAP_DUMB failed\n"); exit(2);
  }
  uint8_t* p = (uint8_t*)drm_mmap_bo(0, (long)m.offset);
  if (!p) { fprintf(stderr, "drm_mmap_bo NULL\n"); exit(2); }
  // Distinct non-black gradient per BO so the two presents are distinguishable.
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++) {
      uint8_t* q = p + (size_t)y * c.pitch + (size_t)x * 4;
      q[0] = (uint8_t)(x * 7 + 1);        // B
      q[1] = (uint8_t)(y * 3 + 1);        // G
      q[2] = 0x40;                        // R
      q[3] = 0xff;
    }
  if (out_pitch) *out_pitch = c.pitch;
  return c.handle;
}

static uint32_t make_fb(uint32_t handle, uint32_t w, uint32_t h, uint32_t pitch) {
  struct drm_mode_fb_cmd2 fb; memset(&fb, 0, sizeof fb);
  fb.fb_id = 0;
  fb.width = w; fb.height = h;
  fb.pixel_format = DRM_FORMAT_XRGB8888;
  fb.handles[0] = handle;
  fb.pitches[0] = pitch;
  if (drm_ioctl(0, DRM_IOCTL_MODE_ADDFB2, (unsigned long)&fb) != 0) {
    fprintf(stderr, "ADDFB2 failed\n"); exit(2);
  }
  return fb.fb_id;
}

static void atomic_commit_two_planes(uint32_t fb1, uint32_t fb2) {
  uint32_t objs[2]   = { 1, 2 };
  uint32_t cnts[2]   = { 2, 2 };
  uint32_t props[4]  = { PROP_CRTC_ID, PROP_FB_ID, PROP_CRTC_ID, PROP_FB_ID };
  uint64_t vals[4]   = { 1, fb1, 2, fb2 };
  struct drm_mode_atomic a;
  memset(&a, 0, sizeof a);
  a.count_objs = 2;
  a.objs_ptr = (uint64_t)(uintptr_t)objs;
  a.count_props_ptr = (uint64_t)(uintptr_t)cnts;
  a.props_ptr = (uint64_t)(uintptr_t)props;
  a.prop_values_ptr = (uint64_t)(uintptr_t)vals;
  if (drm_ioctl(0, DRM_IOCTL_MODE_ATOMIC, (unsigned long)&a) != 0) {
    fprintf(stderr, "MODE_ATOMIC failed\n"); exit(2);
  }
}

// ---- evdev part -----------------------------------------------------------------------
#define EVDEV_TYPE 'E'
#define EVIOCGNAME(len) ((2UL<<30)|(EVDEV_TYPE<<8)|0x06|((len)<<16))
#define EVIOCGID   ((2UL<<30)|(EVDEV_TYPE<<8)|0x02|(8<<16))
#define EVIOCGBIT(ev,len) ((2UL<<30)|(EVDEV_TYPE<<8)|(0x20+(ev))|((len)<<16))

struct linux_input_id { uint16_t bustype, vendor, product, version; };
struct linux_input_event { int64_t tv_sec, tv_usec; uint16_t type, code; int32_t value; };

static int evdev_part(void) {
  int fd = evdev_open_node("/dev/input/event0");
  if (fd < 0) { fprintf(stderr, "EVDEV open failed\n"); return 1; }

  char name[64]; memset(name, 0, sizeof name);
  if (evdev_ioctl(fd, EVIOCGNAME(sizeof name), (unsigned long)name) != 0) {
    fprintf(stderr, "EVDEV EVIOCGNAME failed\n"); return 1;
  }
  fprintf(stderr, "EVDEV name=%s\n", name);

  struct linux_input_id id;
  if (evdev_ioctl(fd, EVIOCGID, (unsigned long)&id) != 0) {
    fprintf(stderr, "EVDEV EVIOCGID failed\n"); return 1;
  }
  fprintf(stderr, "EVDEV id bus=0x%x vendor=0x%x product=0x%x\n", id.bustype, id.vendor, id.product);

  unsigned char ev[32], key[128];
  if (evdev_ioctl(fd, EVIOCGBIT(0, sizeof ev), (unsigned long)ev) != 0) {
    fprintf(stderr, "EVDEV EVIOCGBIT(0) failed\n"); return 1;
  }
  if (evdev_ioctl(fd, EVIOCGBIT(1, sizeof key), (unsigned long)key) != 0) {
    fprintf(stderr, "EVDEV EVIOCGBIT(EV_KEY) failed\n"); return 1;
  }
  int has_key_a = (key[30 >> 3] >> (30 & 7)) & 1;   // KEY_A
  fprintf(stderr, "EVDEV EV_KEY bit KEY_A(30)=%d (EV_KEY cap=%d)\n", has_key_a, (ev[1 >> 3] >> 1) & 1);

  // Inject a key-down + SYN, then read back two 24-byte records.
  evdev_inject(EVDEV_DEV_KBD, 1 /*EV_KEY*/, 30 /*KEY_A*/, 1);
  evdev_syn(EVDEV_DEV_KBD);
  struct linux_input_event in[4];
  ssize_t n = read(fd, in, sizeof in);
  if (n < 48) { fprintf(stderr, "EVDEV read got %zd bytes (want >=48)\n", n); return 1; }
  fprintf(stderr, "EVDEV read %zd bytes: type=%u code=%u value=%d | type=%u code=%u value=%d\n",
          n, in[0].type, in[0].code, in[0].value, in[1].type, in[1].code, in[1].value);
  if (in[0].type != 1 || in[0].code != 30 || in[0].value != 1) { fprintf(stderr, "EVDEV key record wrong\n"); return 1; }
  if (in[1].type != 0 || in[1].code != 0) { fprintf(stderr, "EVDEV SYN record wrong\n"); return 1; }
  return 0;
}

int main(void) {
  uint32_t p1 = 0, p2 = 0;
  uint32_t h1 = make_bo(3440, 1440, &p1);
  uint32_t h2 = make_bo(3600, 2338, &p2);
  uint32_t fb1 = make_fb(h1, 3440, 1440, p1);
  uint32_t fb2 = make_fb(h2, 3600, 2338, p2);
  atomic_commit_two_planes(fb1, fb2);
  fprintf(stderr, "ATOMIC committed fb1=%u fb2=%u (expect DRM_PRESENT crtc=1 AND crtc=2 above)\n", fb1, fb2);

  int evrc = evdev_part();
  fprintf(stderr, "RESULT evdev=%s\n", evrc ? "FAIL" : "PASS");
  return evrc;
}
