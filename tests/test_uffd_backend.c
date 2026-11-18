#include "uffd_backend.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

typedef struct { uffd_backend *b; uint64_t address; int result; } fault_args;
static void *report(void *v) {
  fault_args *a = v;
  a->result = uffd_backend_report_fault(a->b, a->address, 1);
  return NULL;
}
static int ready(uffd_backend *b, int wait_ms) {
  struct pollfd p = { .fd = uffd_backend_poll_fd(b), .events = POLLIN };
  int n = poll(&p, 1, wait_ms);
  assert(n >= 0);
  return n && (p.revents & POLLIN);
}

int main(void) {
  uffd_backend *b = uffd_backend_create();
  assert(b && uffd_backend_poll_fd(b) >= 0);
  assert(uffd_backend_register(b, 0x10000, 0x8000) == 0);
  assert(uffd_backend_register(b, 0x18000, 0x4000) == 0);
  assert(uffd_backend_register(b, 0x14000, 0x4000) == -1 && errno == EBUSY);
  assert(uffd_backend_register(b, 0x12345, 0x4000) == -1 && errno == EINVAL);
  assert(uffd_backend_register(b, UINT64_MAX - 0x3fff, 0x4000) == -1 && errno == EINVAL);
  assert(!ready(b, 0));
  uffd_backend_event e;
  assert(uffd_backend_read_event(b, &e) == 0 && errno == EAGAIN);
  assert(uffd_backend_report_fault(b, 0x20000, 0) == 0);

  fault_args a = {b, 0x10027, -9};
  pthread_t t;
  assert(pthread_create(&t, NULL, report, &a) == 0);
  assert(ready(b, 2000));
  assert(uffd_backend_read_event(b, &e) == 1);
  assert(e.address == a.address && e.flags == 1 && e.sequence != 0);
  assert(!ready(b, 0));
  assert(uffd_backend_complete(b, e.sequence) == 0);
  assert(pthread_join(t, NULL) == 0 && a.result == 1);
  assert(uffd_backend_complete(b, e.sequence) == -1 && errno == ENOENT);

  // Several faulting guest threads must each get an event and an independent
  // completion, including when the resolver handles them in reverse order.
  fault_args batch[8];
  pthread_t threads[8];
  uint64_t seq[8];
  for (unsigned i = 0; i < 8; ++i) {
    // Alternate between the first and second registered ranges.
    batch[i] = (fault_args){b, (i & 1) ? 0x18010 + (uint64_t)(i / 2) * 0x100 :
                                      0x10010 + (uint64_t)(i / 2) * 0x100, -9};
    assert(pthread_create(&threads[i], NULL, report, &batch[i]) == 0);
  }
  for (unsigned i = 0; i < 8; ++i) {
    assert(ready(b, 2000));
    assert(uffd_backend_read_event(b, &e) == 1);
    seq[i] = e.sequence;
    if (i) assert(seq[i] > seq[i - 1]);
  }
  assert(!ready(b, 0));
  for (unsigned i = 8; i; --i) assert(uffd_backend_complete(b, seq[i - 1]) == 0);
  for (unsigned i = 0; i < 8; ++i)
    assert(pthread_join(threads[i], NULL) == 0 && batch[i].result == 1);

  // Unregister removes a queued notification and cancels its stopped reporter.
  fault_args c = {b, 0x18001, -9};
  assert(pthread_create(&t, NULL, report, &c) == 0);
  assert(ready(b, 2000));
  assert(uffd_backend_unregister(b, 0x18000, 0x4000) == 0);
  assert(pthread_join(t, NULL) == 0 && c.result == 0);
  assert(!ready(b, 0));
  assert(uffd_backend_read_event(b, &e) == 0 && errno == EAGAIN);
  assert(uffd_backend_unregister(b, 0x18000, 0x4000) == -1 && errno == ENOENT);

  // Close cancels a stopped reporter and waits for it to exit before freeing.
  fault_args d = {b, 0x14001, -9};
  assert(pthread_create(&t, NULL, report, &d) == 0);
  assert(ready(b, 2000));
  uffd_backend_destroy(b);
  assert(pthread_join(t, NULL) == 0 && d.result == 0);
  puts("UFFD_BACKEND_LIFECYCLE_OK");
  return 0;
}
