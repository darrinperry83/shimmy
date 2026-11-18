#include "mqueue_core.h"
#include "mqueue_fd_registry.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  struct mq_core_handle *source = NULL, *other = NULL, *borrowed = NULL;
  assert(mq_core_open("/shimmy-mq-fd-test", 1, 1, 2, 8, &source) == 0);
  assert(mqfd_bind(100, source, 2, 0, 1) == 0); // ownership moves to registry
  assert(mqfd_bind(100, source, 2, 0, 1) == EEXIST);
  assert(mqfd_duplicate(100, 101, 0) == 0);
  int mode = -1, nonblock = -1, cloexec = -1;
  assert(mqfd_acquire(101, &borrowed, &mode, &nonblock, &cloexec) == 0);
  assert(mode == 2 && nonblock == 0 && cloexec == 0);
  assert(mqfd_acquire(100, &source, NULL, NULL, &cloexec) == 0);
  assert(cloexec == 1);
  mq_core_close(source);
  assert(mqfd_set_nonblock(101, 1) == 0);
  assert(mqfd_set_cloexec(101, 1) == 0);
  assert(mqfd_acquire(100, &source, NULL, &nonblock, &cloexec) == 0);
  assert(nonblock == 1 && cloexec == 1);
  assert(mqfd_set_cloexec(100, 0) == 0);
  assert(mqfd_acquire(101, &other, NULL, NULL, &cloexec) == 0);
  assert(cloexec == 1);
  mq_core_close(other);
  mq_core_close(source);

  // A lookup pins the queue independently of descriptor close.
  assert(mqfd_detach(100) == 0);
  assert(mqfd_detach(101) == 0);
  assert(mqfd_acquire(101, &source, NULL, NULL, NULL) == EBADF);
  assert(mq_core_send(borrowed, "held", 4, 1) == 0);
  char out[8] = {0};
  size_t length = 0;
  assert(mq_core_receive(borrowed, out, sizeof out, &length, NULL) == 0);
  assert(length == 4 && memcmp(out, "held", 4) == 0);
  mq_core_close(borrowed);

  assert(mq_core_open("/shimmy-mq-fd-test", 0, 0, 0, 0, &source) == 0);
  assert(mqfd_bind(102, source, 0, 0, 0) == 0);
  assert(mq_core_open("/shimmy-mq-fd-other", 1, 1, 1, 8, &other) == 0);
  assert(mqfd_bind(103, other, 1, 0, 0) == 0);
  assert(mqfd_duplicate(102, 103, 1) == 0); // replace occupied alias
  assert(mqfd_acquire(103, &borrowed, &mode, NULL, &cloexec) == 0);
  assert(mode == 0 && cloexec == 1);
  mq_core_close(borrowed);
  assert(mqfd_detach(102) == 0);
  assert(mqfd_detach(103) == 0);
  assert(mq_core_unlink("/shimmy-mq-fd-test") == 0);
  assert(mq_core_unlink("/shimmy-mq-fd-other") == 0);
  puts("mqueue fd registry PASS");
  return 0;
}
