#include "uffd_linux_abi.h"
#include <errno.h>
#include <string.h>

enum { RD_UFFD_PAGE_SIZE = 16384 };

_Static_assert(sizeof(rd_uffd_msg) == 32, "Linux uffd_msg size");
_Static_assert(offsetof(rd_uffd_msg, arg.pagefault.flags) == 8, "Linux fault flags offset");
_Static_assert(offsetof(rd_uffd_msg, arg.pagefault.address) == 16, "Linux fault address offset");
_Static_assert(sizeof(rd_uffdio_api) == 24, "Linux uffdio_api size");
_Static_assert(sizeof(rd_uffdio_register) == 32, "Linux uffdio_register size");

int rd_uffd_check_api(const rd_uffdio_api *request) {
  if (!request) { errno = EFAULT; return -1; }
  if (request->api != RD_UFFD_API || request->features) {
    errno = EINVAL; return -1;
  }
  return 0;
}

int rd_uffd_register_missing(uffd_backend *b, rd_uffdio_register *request,
                             int (*eligible)(uint64_t, uint64_t, void *), void *ctx) {
  if (!b || !request) { errno = EFAULT; return -1; }
  if (request->mode != RD_UFFD_REGISTER_MODE_MISSING) {
    errno = EOPNOTSUPP; return -1;
  }
  uint64_t start = request->range.start, len = request->range.len;
  if (!start || !len || (start & (RD_UFFD_PAGE_SIZE - 1)) ||
      (len & (RD_UFFD_PAGE_SIZE - 1)) || start > UINT64_MAX - len) {
    errno = EINVAL; return -1;
  }
  // The current guest memory manager has no reliable untouched-anonymous-page
  // predicate. Without one the registration must refuse rather than steal
  // already-populated pages or promise faults that cannot be delivered.
  if (!eligible || !eligible(start, len, ctx)) {
    errno = EOPNOTSUPP; return -1;
  }
  if (uffd_backend_register(b, start, len) < 0) return -1;
  request->ioctls = 0; // no page-resolution ioctls are implemented yet
  return 0;
}

int rd_uffd_unregister_exact(uffd_backend *b, const rd_uffdio_range *range) {
  if (!range) { errno = EFAULT; return -1; }
  return uffd_backend_unregister(b, range->start, range->len);
}

int rd_uffd_pack_pagefault(const uffd_backend_event *event, rd_uffd_msg *out) {
  if (!event || !out) { errno = EFAULT; return -1; }
  if (event->flags & ~((uint64_t)RD_UFFD_PAGEFAULT_FLAG_WRITE)) {
    errno = EOPNOTSUPP; return -1;
  }
  memset(out, 0, sizeof *out);
  out->event = RD_UFFD_EVENT_PAGEFAULT;
  out->arg.pagefault.flags = event->flags;
  out->arg.pagefault.address = event->address & ~(uint64_t)(RD_UFFD_PAGE_SIZE - 1);
  return 0;
}
