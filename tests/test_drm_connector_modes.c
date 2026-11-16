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
  // libdrm realpath()s /sys/dev/char/226:* to /sys/class/drm/<node>.
  // Both spellings must report one PCI identity before Mesa selects a driver.
  for (int minor = 0; minor <= 128; minor += 128) {
    const char* node = minor ? "renderD128" : "card0";
    char char_path[128], class_path[128];
    const char* leaves[] = { "drm", "uevent", "vendor", "device",
                             "subsystem_vendor", "subsystem_device" };
    for (size_t i = 0; i < sizeof leaves / sizeof leaves[0]; i++) {
      snprintf(char_path, sizeof char_path, "/sys/dev/char/226:%d/device/%s", minor, leaves[i]);
      snprintf(class_path, sizeof class_path, "/sys/class/drm/%s/device/%s", node, leaves[i]);
      assert(drm_sysfs_kind(char_path) == drm_sysfs_kind(class_path));
      if (drm_sysfs_kind(char_path) == 5 || drm_sysfs_kind(char_path) == 6)
        assert(strcmp(drm_sysfs_pci_content(char_path),
                      drm_sysfs_pci_content(class_path)) == 0);
    }
    snprintf(class_path, sizeof class_path, "/sys/class/drm/%s/device", node);
    assert(drm_sysfs_kind(class_path) == 3);
  }
  assert(drm_sysfs_kind("/sys/class/drm/card1/device/vendor") == 0);
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
