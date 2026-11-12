#include "timerfd_shim.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

int main(void) {
  assert(timerfd_create_shim(99, 0) == -22);
  int fd = (int)timerfd_create_shim(1, 0x80800);
  assert(fd >= 0);
  struct pollfd p = {.fd = fd, .events = POLLIN};
  assert(poll(&p, 1, 0) == 0);
  linux_itimerspec next = {.it_value = {.tv_sec = 0, .tv_nsec = 20000000}};
  assert(timerfd_settime_shim(fd, 0, &next, NULL) == 0);
  linux_itimerspec current;
  assert(timerfd_gettime_shim(fd, &current) == 0);
  assert(current.it_value.tv_nsec > 0);
  assert(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN));
  uint64_t ticks = 0;
  assert(read(fd, &ticks, sizeof ticks) == sizeof ticks && ticks == 1);
  struct timespec now;
  clock_gettime(CLOCK_UPTIME_RAW, &now);
  linux_itimerspec expired = {.it_value = {.tv_sec = now.tv_sec - 1, .tv_nsec = now.tv_nsec}};
  assert(timerfd_settime_shim(fd, 1, &expired, NULL) == 0);
  assert(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN));
  assert(read(fd, &ticks, sizeof ticks) == sizeof ticks && ticks == 1);
  clock_gettime(CLOCK_UPTIME_RAW, &now);
  linux_itimerspec imminent = {.it_value = {.tv_sec = now.tv_sec, .tv_nsec = now.tv_nsec + 20000000}};
  if (imminent.it_value.tv_nsec >= 1000000000) {
    imminent.it_value.tv_sec++;
    imminent.it_value.tv_nsec -= 1000000000;
  }
  assert(timerfd_settime_shim(fd, 1, &imminent, NULL) == 0);
  assert(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN));
  assert(read(fd, &ticks, sizeof ticks) == sizeof ticks && ticks == 1);
  assert(poll(&p, 1, 0) == 0);
  // Rearming drops an old unread expiration, as Linux timerfd does.
  clock_gettime(CLOCK_UPTIME_RAW, &now);
  expired.it_value.tv_sec = now.tv_sec - 1;
  expired.it_value.tv_nsec = now.tv_nsec;
  assert(timerfd_settime_shim(fd, 1, &expired, NULL) == 0);
  assert(poll(&p, 1, 1000) == 1);
  next.it_value.tv_sec = 0;
  next.it_value.tv_nsec = 300000000;
  assert(timerfd_settime_shim(fd, 0, &next, NULL) == 0);
  assert(poll(&p, 1, 0) == 0);
  assert(timerfd_settime_shim(fd, 0, &next, NULL) == 0);
  assert(poll(&p, 1, 1000) == 1);
  assert(read(fd, &ticks, sizeof ticks) == sizeof ticks && ticks == 1);
  timerfd_shim_closed(fd);
  close(fd);
  assert(timerfd_gettime_shim(fd, &current) == -9);
  puts("PASS: timerfd one-shot readiness, rearm, expired absolute, read, and close");
  return 0;
}
