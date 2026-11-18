#include "mqueue_shared.h"

#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

// This is the exact proposed shortcut: return mq_shared_open's regular-file
// descriptor to the guest as mqd_t. The file is pollable, but its readiness is
// not the queue's readiness. Keep this test until a real guest fd adapter exists.
int main(void) {
  char dir[] = "/tmp/shimmy-mq-fd-blocker.XXXXXX";
  assert(mkdtemp(dir));
  int fd = -1;
  assert(mq_shared_open(dir, "/poll", 1, 1, 1, 8, &fd) == 0);
  uint32_t magic = 0;
  assert(pread(fd, &magic, sizeof magic, 0) == sizeof magic);
  assert(magic == 0x534d5131u); // Raw fd exposes private queue metadata.
  struct pollfd p = { .fd = fd, .events = POLLIN | POLLOUT };
  int readable = -1, writable = -1;
  assert(mq_shared_status(fd, NULL, NULL, NULL, &readable, &writable) == 0);
  assert(readable == 0 && writable == 1);
  assert(poll(&p, 1, 0) == 1);
  assert((p.revents & POLLIN) != 0); // Incorrect for an empty Linux mqueue.

  assert(mq_shared_send(fd, "x", 1, 0) == 0);
  assert(mq_shared_status(fd, NULL, NULL, NULL, &readable, &writable) == 0);
  assert(readable == 1 && writable == 0);
  p.revents = 0;
  assert(poll(&p, 1, 0) == 1);
  assert((p.revents & POLLOUT) != 0); // Incorrect for a full Linux mqueue.

  assert(mq_shared_close(fd) == 0);
  assert(mq_shared_unlink(dir, "/poll") == 0);
  char lockfile[256];
  snprintf(lockfile, sizeof lockfile, "%s/.lock", dir);
  assert(unlink(lockfile) == 0);
  assert(rmdir(dir) == 0);
  puts("mqueue raw backing fd poll mismatch reproduced");
  return 0;
}
