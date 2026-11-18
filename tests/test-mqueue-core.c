#include "mqueue_core.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  struct mq_core_handle *a = NULL, *b = NULL, *replacement = NULL;
  assert(mq_core_open("invalid", 1, 0, 2, 8, &a) == EINVAL);
  assert(mq_core_open("/a/b", 1, 0, 2, 8, &a) == EINVAL);
  assert(mq_core_open("/shimmy-mq-core-test", 0, 0, 0, 0, &a) == ENOENT);
  assert(mq_core_open("/shimmy-mq-core-test", 1, 1, 2, 8, &a) == 0);
  assert(mq_core_open("/shimmy-mq-core-test", 1, 1, 2, 8, &b) == EEXIST);
  assert(mq_core_open("/shimmy-mq-core-test", 0, 0, 0, 0, &b) == 0);

  char out[8] = {0};
  size_t length = 0, count = 0;
  unsigned priority = 0;
  int readable = 0, writable = 0;
  assert(mq_core_status(a, &count, NULL, NULL, &readable, &writable) == 0);
  assert(count == 0 && !readable && writable);
  assert(mq_core_receive(a, out, sizeof out, &length, &priority) == EAGAIN);
  assert(mq_core_send(a, "low", 3, 1) == 0);
  assert(mq_core_send(b, "high", 4, 7) == 0);
  assert(mq_core_send(a, "full", 4, 1) == EAGAIN);
  assert(mq_core_status(b, &count, NULL, NULL, &readable, &writable) == 0);
  assert(count == 2 && readable && !writable);
  assert(mq_core_receive(a, out, 4, &length, &priority) == EMSGSIZE);
  assert(mq_core_receive(a, out, sizeof out, &length, &priority) == 0);
  assert(length == 4 && priority == 7 && memcmp(out, "high", 4) == 0);
  assert(mq_core_receive(b, out, sizeof out, &length, &priority) == 0);
  assert(length == 3 && priority == 1 && memcmp(out, "low", 3) == 0);
  assert(mq_core_send(a, "oversized!", 10, 1) == EMSGSIZE);
  assert(mq_core_send(a, "invalid", 7, 32768) == EINVAL);
  assert(mq_core_send(a, "one", 3, 4) == 0);
  assert(mq_core_send(a, "two", 3, 4) == 0);
  assert(mq_core_receive(b, out, sizeof out, &length, &priority) == 0);
  assert(priority == 4 && memcmp(out, "one", 3) == 0);
  assert(mq_core_receive(b, out, sizeof out, &length, &priority) == 0);
  assert(priority == 4 && memcmp(out, "two", 3) == 0);

  assert(mq_core_unlink("/shimmy-mq-core-test") == 0);
  assert(mq_core_open("/shimmy-mq-core-test", 0, 0, 0, 0, &replacement) == ENOENT);
  assert(mq_core_send(a, "old", 3, 2) == 0); // unlinked open handle survives
  assert(mq_core_open("/shimmy-mq-core-test", 1, 1, 1, 8, &replacement) == 0);
  assert(mq_core_receive(replacement, out, sizeof out, &length, NULL) == EAGAIN);
  assert(mq_core_receive(b, out, sizeof out, &length, &priority) == 0);
  assert(length == 3 && memcmp(out, "old", 3) == 0);
  mq_core_close(a);
  mq_core_close(b);
  assert(mq_core_unlink("/shimmy-mq-core-test") == 0);
  mq_core_close(replacement);
  puts("mqueue core PASS");
  return 0;
}
