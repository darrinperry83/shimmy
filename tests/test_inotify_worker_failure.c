#include "inotify_watch.h"

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/event.h>
#include <unistd.h>

int main(void) {
  int pipefd[2];
  assert(pipe(pipefd) == 0);
  int kq = kqueue();
  assert(kq >= 0);
  assert(inotify_watch_register(pipefd[0], pipefd[1], kq, NULL, NULL) == 0);
  char path[] = "/tmp/shimmy-inotify-worker-XXXXXX";
  int file = mkstemp(path);
  assert(file >= 0);
  assert(inotify_watch_add(pipefd[0], path, 2) > 0);
  // Force an invalid worker queue. The worker may be blocked inside kevent;
  // a write wakes it, and the following kevent must terminate on EBADF.
  close(kq);
  assert(write(file, "x", 1) == 1);
  int saw_failure = 0;
  for (int attempt = 0; attempt < 200; attempt++) {
    long r = inotify_watch_add(pipefd[0], path, 2);
    if (r == -5) { saw_failure = 1; break; }
    usleep(1000);
  }
  assert(saw_failure);
  close(file);
  unlink(path);
  puts("inotify worker exits on invalid queue: OK");
}
