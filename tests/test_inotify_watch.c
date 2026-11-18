#include "inotify_watch.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <unistd.h>

struct event { int32_t wd; uint32_t mask, cookie, len; };

int main(void) {
  int a[2], b[2];
  assert(pipe(a) == 0 && pipe(b) == 0);
  assert(inotify_watch_register(a[0], a[1], kqueue(), NULL, NULL) == 0);
  assert(inotify_watch_register(b[0], b[1], kqueue(), NULL, NULL) == 0);
  char path[] = "/tmp/shimmy-inotify-watch-XXXXXX";
  int file = mkstemp(path);
  assert(file >= 0);
  close(file);

  int wd = (int)inotify_watch_add(a[0], path, 0x2);
  assert(wd > 0);
  assert(inotify_watch_add(a[0], path, 0x4) == wd); // duplicate path keeps wd
  assert(inotify_watch_add(a[0], path, 0x01000002) == -ENOTDIR);
  assert(inotify_watch_add(a[0], path, 0x10000000) == -EEXIST);
  assert(inotify_watch_remove(b[0], wd) == -EINVAL); // wd is scoped to instance
  assert(inotify_watch_remove(a[0], wd) == 0);
  assert(inotify_watch_remove(a[0], wd) == -EINVAL); // removal changes state

  struct event ev;
  assert(read(a[0], &ev, sizeof ev) == sizeof ev);
  assert(ev.wd == wd && ev.mask == 0x8000 && ev.cookie == 0 && ev.len == 0);
  int copy = dup(a[0]);
  assert(copy >= 0);
  assert(inotify_watch_add(copy, path, 0x2) > wd); // fd alias shares instance
  assert(inotify_watch_remove(-1, wd) == -EBADF);
  assert(inotify_watch_remove(STDOUT_FILENO, wd) == -EINVAL);
  assert(inotify_watch_add(a[0], "/tmp/shimmy-no-such-watch-path", 0x2) == -ENOENT);

  unlink(path);
  close(copy);
  close(a[0]); close(a[1]); close(b[0]); close(b[1]);
  puts("inotify watch removal state: OK");
  return 0;
}
