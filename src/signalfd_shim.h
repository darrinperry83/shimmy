#ifndef RD_SIGNALFD_SHIM_H
#define RD_SIGNALFD_SHIM_H
#include <stddef.h>
#include <stdint.h>
// Isolated Weston probe: supports the standard signals requested by its event loop.
long signalfd4_shim(int fd, const uint64_t* mask, size_t size, int flags);
void signalfd_shim_closed(int fd);
#endif
