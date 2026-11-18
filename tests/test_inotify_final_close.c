#include "inotify_watch.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/event.h>
#include <unistd.h>

int main(void) {
  int fds[2];
  assert(pipe(fds) == 0);
  int kq = kqueue();
  assert(kq >= 0);
  assert(inotify_watch_register(fds[0], fds[1], kq, NULL, NULL) == 0);
  char path[] = "/tmp/shimmy-inotify-close-XXXXXX";
  int file = mkstemp(path);
  assert(file >= 0);
  assert(inotify_watch_add(fds[0], path, 2) > 0);

  int copy = dup(fds[0]);
  assert(copy >= 0);
  inotify_watch_dup(fds[0], copy);
  assert(inotify_watch_close(fds[0]) == 1);
  assert(fcntl(fds[0], F_GETFD) == -1 && errno == EBADF);
  assert(write(file, "x", 1) == 1);
  struct pollfd p = { copy, POLLIN, 0 };
  assert(poll(&p, 1, 3000) == 1);
  char event[16];
  assert(read(copy, event, sizeof event) == sizeof event);
  assert(inotify_watch_close(copy) == 1);
  assert(fcntl(copy, F_GETFD) == -1 && errno == EBADF);
  assert(fcntl(fds[1], F_GETFD) == -1 && errno == EBADF);
  assert(fcntl(kq, F_GETFD) == -1 && errno == EBADF);
  assert(inotify_watch_close(copy) == 0);
  close(file);
  unlink(path);
  puts("inotify final close tears down worker and fds: OK");
}
