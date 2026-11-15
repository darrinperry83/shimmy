/* Build as an aarch64 Linux static PIE and run through shim_cli in a test root. */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

int main(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_BOOTTIME, &now) < 0) return 1;
  int fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK);
  if (fd < 0) return 2;
  struct itimerspec spec = {0};
  spec.it_value.tv_sec = now.tv_sec;
  spec.it_value.tv_nsec = now.tv_nsec + 100000000;
  if (spec.it_value.tv_nsec >= 1000000000) {
    spec.it_value.tv_sec++;
    spec.it_value.tv_nsec -= 1000000000;
  }
  if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &spec, NULL) < 0) return 3;
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  if (poll(&pfd, 1, 2000) != 1 || !(pfd.revents & POLLIN)) return 4;
  uint64_t expirations = 0;
  if (read(fd, &expirations, sizeof expirations) != sizeof expirations || expirations != 1) return 5;
  if (timerfd_create(99, 0) != -1 || errno != EINVAL) return 6;
  close(fd);
  puts("BOOTTIME_TIMERFD_PASS");
  return 0;
}
