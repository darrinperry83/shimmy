#include <assert.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/drmasahi.c"

int shim_fd_reserve(int fd) { return fd; }
void shim_fd_unreserve(int fd) { (void)fd; }

int rdgpu_drm_connector_output(uint32_t connector, uint32_t *w, uint32_t *h) {
  (void)connector; (void)w; (void)h; return -1;
}
void rdgpu_handoff_revoke(dev_t device, ino_t inode) {
  (void)device; (void)inode;
}

static unsigned long request(unsigned nr, unsigned dir) {
  return ((unsigned long)DRM_TYPE << IOC_TYPESHIFT) |
         ((unsigned long)nr << IOC_NRSHIFT) |
         ((unsigned long)dir << IOC_DIRSHIFT) |
         (4ul << IOC_SIZESHIFT);
}

int main(void) {
  char group[] = "/tmp/shimmy-drm-magic-XXXXXX";
  assert(mkdtemp(group));
  assert(setenv("RDSOCKETGROUP", group, 1) == 0);
  int master = drm_open_fd(), client = drm_open_fd(), other = drm_open_fd();
  int render = drm_open_fd();
  assert(master >= 0 && client >= 0 && other >= 0 && render >= 0);
  drm_register_fd_path(master, "/dev/dri/card0");
  drm_register_fd_path(client, "/dev/dri/card0");
  drm_register_fd_path(other, "/dev/dri/card0");
  drm_register_fd_path(render, "/dev/dri/renderD128");
  uint32_t probe = 0;
  assert(drm_ioctl(render, request(DRM_NR_GET_MAGIC, 2), (uintptr_t)&probe) == -EINVAL);
  assert(drm_ioctl(master, request(DRM_NR_AUTH_MAGIC, 1), (uintptr_t)&probe) == -EINVAL);
  assert(drm_ioctl(other, request(DRM_NR_AUTH_MAGIC, 1), (uintptr_t)&probe) == -EACCES);
  assert(drm_ioctl(master, request(DRM_NR_SET_MASTER, 0), 0) == 0);
  uint32_t magic = 0;
  assert(drm_ioctl(client, request(DRM_NR_GET_MAGIC, 2), (uintptr_t)&magic) == 0);
  assert(magic != 0);
  uint32_t repeat = 0;
  assert(drm_ioctl(client, request(DRM_NR_GET_MAGIC, 2), (uintptr_t)&repeat) == 0);
  assert(repeat == magic);
  assert(drm_ioctl(other, request(DRM_NR_AUTH_MAGIC, 1), (uintptr_t)&magic) == -EACCES);
  uint32_t wrong = magic + 1;
  assert(drm_ioctl(master, request(DRM_NR_AUTH_MAGIC, 1), (uintptr_t)&wrong) == -EINVAL);
  assert(drm_ioctl(master, request(DRM_NR_AUTH_MAGIC, 1), (uintptr_t)&magic) == 0);
  assert(drm_ioctl(master, request(DRM_NR_AUTH_MAGIC, 1), (uintptr_t)&magic) == -EINVAL);
  drm_forget_fd(client); close(client);
  client = drm_open_fd(); assert(client >= 0);
  drm_register_fd_path(client, "/dev/dri/card0");
  assert(drm_ioctl(client, request(DRM_NR_GET_MAGIC, 2), (uintptr_t)&magic) == 0);
  drm_forget_fd(client); close(client);
  assert(drm_ioctl(master, request(DRM_NR_AUTH_MAGIC, 1), (uintptr_t)&magic) == -EINVAL);
  drm_forget_fd(master); close(master);
  drm_forget_fd(other); close(other);
  drm_forget_fd(render); close(render);
  DIR *dir = opendir(group); assert(dir);
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char path[256];
    snprintf(path, sizeof path, "%s/%s", group, entry->d_name);
    assert(unlink(path) == 0);
  }
  closedir(dir);
  assert(rmdir(group) == 0);
  puts("DRM magic authentication tests passed");
}
