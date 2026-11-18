#include "inotify_watch.h"

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/event.h>
#include <unistd.h>

struct event { int32_t wd; uint32_t mask, cookie, len; };

static struct event next_event(int fd) {
  struct pollfd p = { fd, POLLIN, 0 };
  assert(poll(&p, 1, 2000) == 1);
  struct event e;
  assert(read(fd, &e, sizeof e) == sizeof e);
  return e;
}

int main(void) {
  int fds[2];
  assert(pipe(fds) == 0);
  assert(inotify_watch_register(fds[0], fds[1], kqueue(), NULL, NULL) == 0);
  char path[] = "/tmp/shimmy-inotify-file-XXXXXX";
  int file = mkstemp(path);
  assert(file >= 0);
  int wd = (int)inotify_watch_add(fds[0], path, 0x2 | 0x400);
  assert(wd > 0);
  assert(write(file, "x", 1) == 1);
  struct event e = next_event(fds[0]);
  assert(e.wd == wd && e.mask == 0x2 && e.len == 0);
  assert(unlink(path) == 0);
  e = next_event(fds[0]);
  assert(e.wd == wd && e.mask == 0x400);
  e = next_event(fds[0]);
  assert(e.wd == wd && e.mask == 0x8000);
  assert(inotify_watch_remove(fds[0], wd) == -22);
  close(file);
  close(fds[0]); close(fds[1]);
  puts("inotify watched-file events: OK");
}
