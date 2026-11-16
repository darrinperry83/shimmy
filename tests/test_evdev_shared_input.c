// Two host processes model PID 1's AppKit sender and an exec'd Linux compositor.
// No GUI, guest root or live input device is touched.
#include "evdev.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct linux_input_event {
  int64_t sec, usec;
  uint16_t type, code;
  int32_t value;
};

// This test has no Linux syscall dispatcher. Production uses the real shim
// descriptor registry; preserve fd identity here to exercise the wire path.
int shim_fd_reserve(int fd) { return fd; }
void shim_fd_unreserve(int fd) { (void)fd; }

static void one_client(int code) {
  int ready[2];
  assert(pipe(ready) == 0);
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    close(ready[0]);
    int fd = evdev_open_node("/dev/input/event0");
    if (fd < 0) _exit(10);
    if (write(ready[1], "r", 1) != 1) _exit(11);
    struct pollfd p = { .fd = fd, .events = POLLIN };
    if (poll(&p, 1, 2000) != 1) _exit(12);
    struct linux_input_event event;
    if (read(fd, &event, sizeof event) != sizeof event) _exit(13);
    if (event.type != EVDEV_EV_KEY || event.code != code || event.value != 1) _exit(14);
    _exit(0);
  }
  close(ready[1]);
  char marker;
  assert(read(ready[0], &marker, 1) == 1);
  close(ready[0]);
  // The child's registration is queued before it reports ready. Host injection
  // drains registration before sending the event; no timing sleep is needed.
  evdev_inject(EVDEV_DEV_KBD, EVDEV_EV_KEY, code, 1);
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  char path[160];
  snprintf(path, sizeof path, "/tmp/rdx-%d/input.%d", (int)getpid(), (int)child);
  assert(unlink(path) == 0);
}

int main(void) {
  char root[32], dir[128], sock[160];
  snprintf(root, sizeof root, "%d", (int)getpid());
  snprintf(dir, sizeof dir, "/tmp/rdx-%s", root);
  snprintf(sock, sizeof sock, "%s/input.sock", dir);
  assert(mkdir(dir, 0700) == 0);
  assert(setenv("RDVT_ROOT_PID", root, 1) == 0);
  assert(setenv("RDNATIVE_SESSION", "1", 1) == 0);
  if (evdev_shared_host_init() != 0) { perror("evdev_shared_host_init"); return 1; }
  // No subscriber is safe and nonblocking. A second compositor replaces the
  // first session's registration after it exits.
  evdev_inject(EVDEV_DEV_KBD, EVDEV_EV_KEY, 30, 1);
  one_client(30);
  one_client(31);
  unlink(sock);
  assert(rmdir(dir) == 0);
  puts("shared evdev input: two-process delivery and replacement passed");
  return 0;
}
