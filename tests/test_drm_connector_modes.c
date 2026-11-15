#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int external_connected = 1;
static uint32_t external_width = 2560, external_height = 1440;
#include "../src/drmasahi.c"

int rdgpu_drm_connector_output(uint32_t connector, uint32_t* w, uint32_t* h) {
  if (connector == 1 && external_connected) {
    *w = external_width; *h = external_height; return 0;
  }
  if (connector == 2) { *w = 3024; *h = 1964; return 0; }
  return -1;
}

void rdgpu_handoff_revoke(dev_t device, ino_t inode) {
  (void)device; (void)inode;
}

static void check_connector(uint32_t id, uint32_t expected_width,
                            uint32_t expected_height, int connected) {
  struct drm_mode_get_connector c = {0};
  struct drm_mode_modeinfo mode = {0};
  c.connector_id = id;
  c.modes_ptr = (uintptr_t)&mode;
  unsigned long request = ((unsigned long)DRM_TYPE << IOC_TYPESHIFT) |
                          ((unsigned long)DRM_NR_MODE_GETCONNECTOR << IOC_NRSHIFT);
  assert(drm_ioctl(-1, request, (uintptr_t)&c) == 0);
  assert(c.connection == (connected ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED));
  assert(c.count_modes == (connected ? 1u : 0u));
  if (connected) {
    assert(mode.hdisplay == expected_width && mode.vdisplay == expected_height);
    assert(mode.clock == ((uint64_t)(expected_width + 80) *
                          (expected_height + 46) * 60 / 1000));
  } else {
    assert(mode.hdisplay == 0 && mode.vdisplay == 0);
  }
}

int main(void) {
  check_connector(1, 2560, 1440, 1);
  check_connector(2, 3024, 1964, 1);
  external_width = 10240; external_height = 4320;
  check_connector(1, 10240, 4320, 1);
  external_width = 2560; external_height = 1440;
  external_connected = 0;
  check_connector(1, 0, 0, 0);
  check_connector(2, 3024, 1964, 1);
  puts("DRM connector mode tests passed");
  return 0;
}
