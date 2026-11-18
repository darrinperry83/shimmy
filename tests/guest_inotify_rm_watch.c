#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
  int fd = (int)syscall(SYS_inotify_init1, IN_NONBLOCK);
  if (fd < 0) { perror("inotify_init1"); return 1; }
  int wd = (int)syscall(SYS_inotify_add_watch, fd, "/watch", IN_MODIFY);
  if (wd <= 0) { perror("inotify_add_watch"); return 2; }
  if (syscall(SYS_inotify_rm_watch, fd, wd) != 0) { perror("first rm_watch"); return 3; }
  struct inotify_event event;
  if (read(fd, &event, sizeof event) != sizeof event ||
      event.wd != wd || event.mask != IN_IGNORED) return 4;
  errno = 0;
  if (syscall(SYS_inotify_rm_watch, fd, wd) != -1 || errno != EINVAL) return 5;
  close(fd);
  puts("guest inotify_rm_watch: OK");
  return 0;
}
