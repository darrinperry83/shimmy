#ifndef SHIMMY_UFFD_BACKEND_H
#define SHIMMY_UFFD_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* Internal, guest-private prototype. No Linux syscall is exposed by this API. */
typedef struct uffd_backend uffd_backend;

typedef struct {
  uint64_t sequence;
  uint64_t address;
  uint64_t flags;
} uffd_backend_event;

uffd_backend *uffd_backend_create(void);
/* Readiness only; callers must get event records through read_event(). */
int uffd_backend_poll_fd(uffd_backend *b);
/* Only 16 KiB aligned, exact, anonymous-missing-mode ranges are modelled. */
int uffd_backend_register(uffd_backend *b, uint64_t start, uint64_t length);
int uffd_backend_unregister(uffd_backend *b, uint64_t start, uint64_t length);
/* Returns 1 after complete(), 0 if unregistered/closed, -1 with errno otherwise. */
int uffd_backend_report_fault(uffd_backend *b, uint64_t address, uint64_t flags);
/* Nonblocking: 1 event, 0 and EAGAIN when empty, -1 for other errors. */
int uffd_backend_read_event(uffd_backend *b, uffd_backend_event *out);
/* Signals a waiting reporter after the caller has populated its page. */
int uffd_backend_complete(uffd_backend *b, uint64_t sequence);
/* Caller must stop other API calls first; concurrent blocked reporters are cancelled. */
void uffd_backend_destroy(uffd_backend *b);

#endif
