#ifndef RD_SIGNALFD_SHIM_H
#define RD_SIGNALFD_SHIM_H
#include <stddef.h>
#include <stdint.h>
// Isolated Weston probe: supports the standard signals requested by its event loop.
long signalfd4_shim(int fd, const uint64_t* mask, size_t size, int flags);
// Retire an active signalfd and its private writer. Returns 1 only when fd
// belonged to this backend; the caller still closes the guest read fd.
int signalfd_shim_closed(int fd);
// Queue one virtual standard signal to a matching signalfd, if any. Returns 1
// when consumed; a full or absent signalfd leaves the caller's pending bit intact.
int signalfd_shim_queue_linux(int signo);
#endif
