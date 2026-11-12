#ifndef RD_TIMERFD_SHIM_H
#define RD_TIMERFD_SHIM_H
#include <time.h>
typedef struct { struct timespec it_interval, it_value; } linux_itimerspec;
long timerfd_create_shim(int clockid, int flags);
long timerfd_settime_shim(int fd, int flags, const linux_itimerspec* next, linux_itimerspec* old);
long timerfd_gettime_shim(int fd, linux_itimerspec* current);
void timerfd_shim_closed(int fd);
#endif
