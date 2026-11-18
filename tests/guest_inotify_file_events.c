#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <unistd.h>

static unsigned next_mask(int fd, int wd) {
  struct pollfd p = { fd, POLLIN, 0 };
  assert(poll(&p, 1, 3000) == 1);
  char buffer[4096];
  ssize_t n = read(fd, buffer, sizeof buffer);
  assert(n >= (ssize_t)sizeof(struct inotify_event));
  struct inotify_event *e = (struct inotify_event *)buffer;
  assert(e->wd == wd && e->len == 0);
  return e->mask;
}

int main(void) {
  int fd = (int)syscall(SYS_inotify_init1, IN_NONBLOCK | IN_CLOEXEC);
  assert(fd >= 0);
  int wd = (int)syscall(SYS_inotify_add_watch, fd, "/watch", IN_MODIFY | IN_DELETE_SELF);
  assert(wd > 0);
  int copy = dup(fd);
  assert(copy >= 0);
  assert(close(fd) == 0);
  int file = open("/watch", O_WRONLY);
  assert(file >= 0 && write(file, "x", 1) == 1 && close(file) == 0);
  assert(next_mask(copy, wd) == IN_MODIFY);
  assert(unlink("/watch") == 0);
  // Both events can be read together; tolerate pipe batching.
  struct pollfd p = { copy, POLLIN, 0 };
  assert(poll(&p, 1, 3000) == 1);
  struct inotify_event e[2];
  ssize_t n = read(copy, e, sizeof e);
  assert(n == (ssize_t)sizeof e);
  assert(e[0].wd == wd && e[0].mask == IN_DELETE_SELF);
  assert(e[1].wd == wd && e[1].mask == IN_IGNORED);
  assert(syscall(SYS_inotify_rm_watch, copy, wd) == -1 && errno == EINVAL);
  assert(close(copy) == 0);
  puts("guest inotify watched-file events: OK");
}
