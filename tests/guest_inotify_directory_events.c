#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void expect_event(int fd, int wd, unsigned mask, const char *name) {
  struct pollfd p = { fd, POLLIN, 0 };
  assert(poll(&p, 1, 3000) == 1);
  unsigned char data[512];
  ssize_t n = read(fd, data, sizeof data);
  assert(n >= (ssize_t)sizeof(struct inotify_event));
  struct inotify_event *e = (struct inotify_event *)data;
  assert(e->wd == wd && e->mask == mask && e->cookie == 0);
  assert(e->len >= strlen(name) + 1 && (e->len & 15) == 0);
  assert(n == (ssize_t)(sizeof *e + e->len));
  assert(strcmp(e->name, name) == 0);
}

int main(void) {
  int fd = (int)syscall(SYS_inotify_init1, IN_NONBLOCK | IN_CLOEXEC);
  assert(fd >= 0);
  int wd = (int)syscall(SYS_inotify_add_watch, fd, "/watchdir", IN_CREATE | IN_DELETE);
  assert(wd > 0);
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    // A forked child has no inherited watcher thread. It must not claim a
    // newly added inherited-instance watch will deliver notifications.
    assert(syscall(SYS_inotify_add_watch, fd, "/watchdir", IN_CREATE) == -1);
    assert(errno == EOPNOTSUPP);
    _exit(0);
  }
  int child_status = 0;
  assert(waitpid(child, &child_status, 0) == child);
  assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
  int copy = dup(fd);
  assert(copy >= 0);
  assert(close(fd) == 0);
  int fcopy = fcntl(copy, F_DUPFD_CLOEXEC, 90);
  assert(fcopy >= 90);
  assert(close(copy) == 0);
  int target = fcopy + 1;
  assert(dup3(fcopy, target, 0) == target);
  assert(close(fcopy) == 0);
  int file = open("/watchdir/alpha", O_CREAT | O_WRONLY, 0600);
  assert(file >= 0);
  expect_event(target, wd, IN_CREATE, "alpha");
  assert(close(file) == 0);
  assert(unlink("/watchdir/alpha") == 0);
  expect_event(target, wd, IN_DELETE, "alpha");
  assert(mkdir("/watchdir/subdir", 0700) == 0);
  expect_event(target, wd, IN_CREATE | IN_ISDIR, "subdir");
  assert(rmdir("/watchdir/subdir") == 0);
  expect_event(target, wd, IN_DELETE | IN_ISDIR, "subdir");
  assert(syscall(SYS_inotify_rm_watch, target, wd) == 0);
  assert(close(target) == 0);
  puts("guest inotify directory events: OK");
}
