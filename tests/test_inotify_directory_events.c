#include "inotify_watch.h"

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>

struct event { int32_t wd; uint32_t mask, cookie, len; };

static void expect_event(int fd, int wd, unsigned mask, const char *name) {
  struct pollfd p = { fd, POLLIN, 0 };
  assert(poll(&p, 1, 3000) == 1);
  unsigned char data[512];
  ssize_t n = read(fd, data, sizeof data);
  assert(n >= (ssize_t)sizeof(struct event));
  struct event *e = (struct event *)data;
  assert(e->wd == wd && e->mask == mask && e->cookie == 0);
  assert(e->len >= strlen(name) + 1 && (e->len & 15) == 0);
  assert(n == (ssize_t)(sizeof *e + e->len));
  assert(strcmp((char *)(data + sizeof *e), name) == 0);
}

int main(void) {
  int fds[2];
  assert(pipe(fds) == 0);
  assert(inotify_watch_register(fds[0], fds[1], kqueue(), NULL, NULL) == 0);
  char path[] = "/tmp/shimmy-inotify-dir-XXXXXX";
  assert(mkdtemp(path));
  int wd = (int)inotify_watch_add(fds[0], path, 0x100 | 0x200);
  assert(wd > 0);
  char child[sizeof path + 16];
  snprintf(child, sizeof child, "%s/alpha", path);
  int file = open(child, O_CREAT | O_WRONLY, 0600);
  assert(file >= 0);
  expect_event(fds[0], wd, 0x100, "alpha");
  close(file);
  assert(unlink(child) == 0);
  expect_event(fds[0], wd, 0x200, "alpha");
  snprintf(child, sizeof child, "%s/subdir", path);
  assert(mkdir(child, 0700) == 0);
  expect_event(fds[0], wd, 0x100 | 0x40000000u, "subdir");
  assert(rmdir(child) == 0);
  expect_event(fds[0], wd, 0x200 | 0x40000000u, "subdir");
  assert(inotify_watch_remove(fds[0], wd) == 0);
  rmdir(path);
  puts("inotify directory create/delete events: OK");
}
