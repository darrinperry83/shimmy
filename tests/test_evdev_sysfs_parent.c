#include "evdev.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int shim_fd_reserve(int fd) { return fd; }
void shim_fd_unreserve(int fd) { (void)fd; }

int main(void) {
  for (int i = 0; i < 2; i++) {
    char parent[80], event[96], path[120];
    snprintf(parent, sizeof parent, "/sys/devices/virtual/input/input%d", i);
    snprintf(event, sizeof event, "%s/event%d", parent, i);
    assert(evdev_sysfs_kind(parent) == 8);
    assert(evdev_sysfs_kind(event) == 6);
    snprintf(path, sizeof path, "%s/uevent", parent);
    assert(evdev_sysfs_kind(path) == 9);
    assert(strstr(evdev_sysfs_content(path), "SUBSYSTEM=input"));
    snprintf(path, sizeof path, "%s/subsystem", parent);
    assert(evdev_sysfs_kind(path) == 10);
    assert(strcmp(evdev_sysfs_link(), "/sys/bus/input") == 0);
    unsigned int mode = 0;
    unsigned long rdev = 0;
    evdev_fake_stat(&mode, &rdev, parent);
    assert(S_ISDIR(mode));
    snprintf(path, sizeof path, "%s/uevent", event);
    assert(evdev_sysfs_kind(path) == 1);
  }
  assert(evdev_sysfs_kind("/sys/devices/virtual/input/input2") == 0);
  assert(evdev_sysfs_kind("/sys/devices/virtual/input/input0/garbage") == 0);
  return 0;
}
