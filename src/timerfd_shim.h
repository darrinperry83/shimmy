#ifndef RD_TIMERFD_SHIM_H
#define RD_TIMERFD_SHIM_H
#include <time.h>
typedef struct { struct timespec it_interval, it_value; } linux_itimerspec;
long timerfd_create_shim(int clockid, int flags);
long timerfd_settime_shim(int fd, int flags, const linux_itimerspec* next, linux_itimerspec* old);
long timerfd_gettime_shim(int fd, linux_itimerspec* current);
// Retire a guest-owned timerfd read end; returns 1 only for a live timerfd.
int timerfd_shim_closed(int fd);
// A real fork keeps the timerfd descriptors but not libdispatch's worker threads.
// Call around the host fork before the child resumes guest instructions.
void timerfd_shim_prepare_fork(void);
void timerfd_shim_parent_after_fork(void);
void timerfd_shim_child_after_fork(void);
#endif
