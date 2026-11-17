#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include "../src/session_gpu_frame.h"

int session_gpu_epoch_parse(const char *text, session_gpu_epoch *out) {
  (void)text; (void)out; return -EINVAL;
}
int session_gpu_child_register_master(pid_t pid, session_gpu_epoch epoch, int fd) {
  (void)pid; (void)epoch; (void)fd; return -ENOSYS;
}
int session_gpu_child_unregister_master(pid_t pid, session_gpu_epoch epoch, int fd) {
  (void)pid; (void)epoch; (void)fd; return -ENOSYS;
}
int session_gpu_child_present_master(pid_t pid, session_gpu_epoch epoch, int fd,
                                     IOSurfaceRef surface, uint64_t generation,
                                     uint32_t crtc, uint32_t width,
                                     uint32_t height, uint32_t format) {
  (void)pid; (void)epoch; (void)fd; (void)surface; (void)generation;
  (void)crtc; (void)width; (void)height; (void)format; return -ENOSYS;
}

static dev_t carrier_dev[2];
static ino_t carrier_ino[2];
static int carrier_state[2]; // 0 pending, 1 completed, 2 revoked
static unsigned carrier_refs[2];
static struct { uint32_t width, height, surface_id; } fake_image = {64, 64, 7};
static IOSurfaceRef image;
static unsigned presents, releases, resolves;
static int present_fail;

static int carrier_index(int fd) {
  struct stat st;
  if (fstat(fd, &st)) return -1;
  for (int i = 0; i < 2; i++)
    if (st.st_dev == carrier_dev[i] && st.st_ino == carrier_ino[i]) return i;
  return -1;
}
int rdgpu_handoff_is_carrier_fd(int fd) { return carrier_index(fd) >= 0; }
int rdgpu_handoff_is_pending_fd(int fd) {
  int i = carrier_index(fd); return i >= 0 && carrier_state[i] == 0;
}
int rdgpu_handoff_frame_ready_fd(int fd, uint64_t* generation) {
  int i = carrier_index(fd);
  if (i < 0 || carrier_state[i] != 1) return 0;
  if (generation) *generation = 1;
  return 1;
}
int rdgpu_handoff_consumer_acquire_fd(int fd, uint64_t generation) {
  int i = carrier_index(fd);
  if (i < 0 || carrier_state[i] != 1 || generation != 1) return -ESTALE;
  carrier_refs[i]++;
  return 0;
}
int rdgpu_handoff_consumer_release_fd(int fd, uint64_t generation) {
  int i = carrier_index(fd);
  if (i < 0 || carrier_state[i] != 1 || generation != 1 || !carrier_refs[i]) return -1;
  carrier_refs[i]--;
  releases++;
  return 0;
}
IOSurfaceRef rdgpu_handoff_resolve_fd(int fd) {
  int i = carrier_index(fd);
  assert(i >= 0 && carrier_state[i] == 1);
  resolves++;
  return image;
}
long gpuwin_present_iosurface_crtc(IOSurfaceRef surface, uint32_t crtc, uint64_t generation) {
  assert(surface == image && crtc == 1 && generation == 1);
  if (present_fail) return -EIO;
  presents++;
  return 0; // test presenter models completed Metal sampling
}

#define IOSurfaceGetWidth(s) (fake_image.width)
#define IOSurfaceGetHeight(s) (fake_image.height)
#define IOSurfaceGetBytesPerRow(s) (fake_image.width * 4)
#define IOSurfaceGetID(s) (fake_image.surface_id)
#define CFRelease(s) ((void)(s))
#define RDGPU_KMS_TEST 1
#include "../src/drmasahi.c"

int shim_fd_reserve(int fd) { return fd; }
void shim_fd_unreserve(int fd) { (void)fd; }
int rdgpu_drm_connector_output(uint32_t connector, uint32_t* w, uint32_t* h) {
  (void)connector; (void)w; (void)h; return -1;
}
void rdgpu_handoff_revoke(dev_t device, ino_t inode) { (void)device; (void)inode; }

static unsigned long request(unsigned nr) {
  return ((unsigned long)DRM_TYPE << IOC_TYPESHIFT) | ((unsigned long)nr << IOC_NRSHIFT);
}
static int carrier(int i) {
  char path[] = "/tmp/shimmy-kms-carrier-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0 && ftruncate(fd, 64 * 64 * 4) == 0);
  unlink(path);
  struct stat st; assert(fstat(fd, &st) == 0);
  carrier_dev[i] = st.st_dev; carrier_ino[i] = st.st_ino;
  return fd;
}
static uint32_t imported_fb(int fd) {
  struct drm_prime_handle ph = {.fd = fd};
  assert(drm_ioctl(-1, request(DRM_NR_PRIME_FD_TO_HANDLE), (uintptr_t)&ph) == 0);
  assert(ph.handle && !bo_find(ph.handle)); // never mapped as CPU BO
  struct drm_mode_fb_cmd2 fb = {.width = 64, .height = 64,
      .pixel_format = DRM_FORMAT_XRGB8888};
  fb.handles[0] = ph.handle; fb.pitches[0] = 64 * 4;
  assert(drm_ioctl(-1, request(DRM_NR_MODE_ADDFB2), (uintptr_t)&fb) == 0);
  struct drm_gem_close close_gem = {.handle = ph.handle};
  assert(drm_ioctl(-1, request(DRM_NR_GEM_CLOSE), (uintptr_t)&close_gem) == 0);
  return fb.fb_id; // framebuffer keeps imported GEM alive after GEM_CLOSE
}

int main(void) {
  setenv("RDGPU_SCANOUT_DIRECT_SOURCE", "1", 1); // exercise retained direct scanout
  image = (IOSurfaceRef)&fake_image;
  int first = carrier(0), second = carrier(1);
  uint32_t fb1 = imported_fb(first);
  struct drm_mode_crtc_page_flip flip = {.crtc_id = 1, .fb_id = fb1};
  assert(drm_ioctl(-1, request(DRM_NR_MODE_PAGE_FLIP), (uintptr_t)&flip) == -EAGAIN);
  assert(presents == 0 && resolves == 0 && g_crtc.fb_id == 0);
  carrier_state[0] = 1;
  assert(drm_ioctl(-1, request(DRM_NR_MODE_PAGE_FLIP), (uintptr_t)&flip) == 0);
  assert(presents == 1 && releases == 0 && carrier_refs[0] == 1 && g_crtc.fb_id == fb1);
  uint32_t fb2 = imported_fb(second);
  flip.fb_id = fb2;
  assert(drm_ioctl(-1, request(DRM_NR_MODE_PAGE_FLIP), (uintptr_t)&flip) == -EAGAIN);
  assert(presents == 1 && releases == 0 && g_crtc.fb_id == fb1);
  uint32_t obj = 1, count = 1, prop = DRM_PROP_FB_ID;
  uint64_t value = fb2;
  struct drm_mode_atomic atomic = {.count_objs = 1, .objs_ptr = (uintptr_t)&obj,
      .count_props_ptr = (uintptr_t)&count, .props_ptr = (uintptr_t)&prop,
      .prop_values_ptr = (uintptr_t)&value};
  assert(drm_ioctl(-1, request(DRM_NR_MODE_ATOMIC), (uintptr_t)&atomic) == -EAGAIN);
  assert(presents == 1 && releases == 0 && g_crtc.fb_id == fb1);
  carrier_state[1] = 1;
  atomic.flags = DRM_MODE_ATOMIC_TEST_ONLY;
  assert(drm_ioctl(-1, request(DRM_NR_MODE_ATOMIC), (uintptr_t)&atomic) == 0);
  assert(g_crtc.fb_id == fb1 && presents == 1);
  atomic.flags = 0;
  present_fail = 1;
  assert(drm_ioctl(-1, request(DRM_NR_MODE_ATOMIC), (uintptr_t)&atomic) == -EIO);
  assert(presents == 1 && releases == 1 && carrier_refs[1] == 0 && g_crtc.fb_id == fb1);
  present_fail = 0;
  assert(drm_ioctl(-1, request(DRM_NR_MODE_ATOMIC), (uintptr_t)&atomic) == 0);
  assert(presents == 2 && releases == 2 && carrier_refs[0] == 0);
  flip.fb_id = fb1;
  assert(drm_ioctl(-1, request(DRM_NR_MODE_PAGE_FLIP), (uintptr_t)&flip) == 0);
  assert(presents == 3 && releases == 3 && carrier_refs[0] == 1);
  assert(drm_ioctl(-1, request(DRM_NR_MODE_PAGE_FLIP), (uintptr_t)&flip) == 0);
  assert(presents == 4 && releases == 3 && carrier_refs[0] == 1); // same scanout held
  unsetenv("RDGPU_SCANOUT_DIRECT_SOURCE"); // windowed ACK ends Metal sampling
  flip.fb_id = fb2;
  assert(drm_ioctl(-1, request(DRM_NR_MODE_PAGE_FLIP), (uintptr_t)&flip) == 0);
  assert(presents == 5 && releases == 5 && !carrier_refs[0] && !carrier_refs[1]);
  flip.fb_id = fb1;
  carrier_state[0] = 2; // revoked carrier cannot flip
  assert(drm_ioctl(-1, request(DRM_NR_MODE_PAGE_FLIP), (uintptr_t)&flip) == -EAGAIN);
  assert(presents == 5 && releases == 5);
  // A recognizable but unauthenticated sparse carrier must never become a CPU BO.
  carrier_state[0] = 2;
  struct drm_prime_handle denied = {.fd = first};
  assert(drm_ioctl(-1, request(DRM_NR_PRIME_FD_TO_HANDLE), (uintptr_t)&denied) == -EACCES);
  close(first); close(second);
  puts("DRM Metal handoff pending, ready, release, and denial tests passed");
}
