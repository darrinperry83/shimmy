#include "uffd_linux_abi.h"
#include <linux/userfaultfd.h>

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <pthread.h>

_Static_assert(sizeof(rd_uffd_msg) == sizeof(struct uffd_msg), "uffd message ABI");
_Static_assert(sizeof(rd_uffdio_api) == sizeof(struct uffdio_api), "uffd API ABI");
_Static_assert(sizeof(rd_uffdio_register) == sizeof(struct uffdio_register), "uffd register ABI");
_Static_assert(offsetof(rd_uffd_msg, arg.pagefault.address) ==
               offsetof(struct uffd_msg, arg.pagefault.address), "fault address ABI");
_Static_assert(RD_UFFD_API == UFFD_API, "API version");
_Static_assert(RD_UFFD_EVENT_PAGEFAULT == UFFD_EVENT_PAGEFAULT, "event number");
_Static_assert(RD_UFFD_REGISTER_MODE_MISSING == UFFDIO_REGISTER_MODE_MISSING,
               "missing mode number");

static int deny(uint64_t start, uint64_t len, void *ctx) {
  (void)start; (void)len; (void)ctx; return 0;
}
static int isolated_fixture(uint64_t start, uint64_t len, void *ctx) {
  (void)ctx; return start == 0x10000 && len == 0x4000;
}
typedef struct { uffd_backend *b; int result; } reporter;
static void *report(void *arg) {
  reporter *r = arg;
  r->result = uffd_backend_report_fault(r->b, 0x12345, RD_UFFD_PAGEFAULT_FLAG_WRITE);
  return NULL;
}

int main(void) {
  rd_uffdio_api api = { .api = RD_UFFD_API };
  assert(rd_uffd_check_api(&api) == 0);
  api.features = UFFD_FEATURE_EVENT_FORK;
  assert(rd_uffd_check_api(&api) == -1 && errno == EINVAL);
  api.features = 0; api.api = 0;
  assert(rd_uffd_check_api(&api) == -1 && errno == EINVAL);
  assert(rd_uffd_check_api(NULL) == -1 && errno == EFAULT);

  uffd_backend *b = uffd_backend_create();
  assert(b);
  rd_uffdio_register reg = { .range = {0x10000, 0x4000},
                             .mode = RD_UFFD_REGISTER_MODE_MISSING,
                             .ioctls = UINT64_MAX };
  assert(rd_uffd_register_missing(b, &reg, NULL, NULL) == -1 && errno == EOPNOTSUPP);
  assert(rd_uffd_register_missing(b, &reg, deny, NULL) == -1 && errno == EOPNOTSUPP);
  assert(reg.ioctls == UINT64_MAX); // refusal must not claim capabilities
  reg.mode = UFFDIO_REGISTER_MODE_WP;
  assert(rd_uffd_register_missing(b, &reg, isolated_fixture, NULL) == -1 && errno == EOPNOTSUPP);
  reg.mode = RD_UFFD_REGISTER_MODE_MISSING;
  reg.range.start++;
  assert(rd_uffd_register_missing(b, &reg, isolated_fixture, NULL) == -1 && errno == EINVAL);
  reg.range.start--;
  assert(rd_uffd_register_missing(b, &reg, isolated_fixture, NULL) == 0);
  assert(reg.ioctls == 0);
  assert(rd_uffd_register_missing(b, &reg, isolated_fixture, NULL) == -1 && errno == EBUSY);

  // Synthetic reporter exercises the event/readiness-to-Linux-wire adapter.
  // It does not assert that a real guest page fault has been intercepted.
  reporter r = {b, -9};
  pthread_t thread;
  assert(pthread_create(&thread, NULL, report, &r) == 0);
  struct pollfd pfd = {.fd = uffd_backend_poll_fd(b), .events = POLLIN};
  assert(poll(&pfd, 1, 2000) == 1 && (pfd.revents & POLLIN));
  uffd_backend_event internal;
  assert(uffd_backend_read_event(b, &internal) == 1);
  rd_uffd_msg wire;
  memset(&wire, 0xff, sizeof wire);
  assert(rd_uffd_pack_pagefault(&internal, &wire) == 0);
  struct uffd_msg official;
  memcpy(&official, &wire, sizeof official);
  assert(official.event == UFFD_EVENT_PAGEFAULT);
  assert(official.arg.pagefault.flags == UFFD_PAGEFAULT_FLAG_WRITE);
  assert(official.arg.pagefault.address == 0x10000);
  assert(official.arg.pagefault.feat.ptid == 0);
  assert(uffd_backend_complete(b, internal.sequence) == 0);
  assert(pthread_join(thread, NULL) == 0 && r.result == 1);
  internal.flags = UFFD_PAGEFAULT_FLAG_WP;
  assert(rd_uffd_pack_pagefault(&internal, &wire) == -1 && errno == EOPNOTSUPP);
  assert(rd_uffd_unregister_exact(b, &reg.range) == 0);
  uffd_backend_destroy(b);
  puts("UFFD_LINUX_ABI_OK");
  return 0;
}
