#ifndef SHIM_POSIX_TIMER_SHIM_H
#define SHIM_POSIX_TIMER_SHIM_H

#include <stdint.h>

// Guest Linux ARM64 syscall 107. This is staged until 108–111 use the same
// registry; callers must not merge it as standalone POSIX-timer support.
long posix_timer_shim_create(uint64_t clockid, uint64_t sigevent_addr,
                             uint64_t timer_id_addr);
long posix_timer_shim_delete(uint64_t timer_id);

#endif
