#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long long ns(const struct timespec *t) {
  return (long long)t->tv_sec * 1000000000LL + t->tv_nsec;
}
static int absolute_sleep(clockid_t clock) {
  struct timespec deadline, after;
  if (clock_gettime(clock, &deadline)) return 1;
  deadline.tv_nsec += 2000000;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  if (syscall(SYS_clock_nanosleep, clock, TIMER_ABSTIME, &deadline,
              (void*)1) != 0) return 2; /* absolute remainder is unused */
  if (clock_gettime(clock, &after) || ns(&after) < ns(&deadline)) return 3;
  deadline.tv_sec = 0;
  deadline.tv_nsec = 0;
  return syscall(SYS_clock_nanosleep, clock, TIMER_ABSTIME, &deadline,
                 NULL) == 0 ? 0 : 4;
}
int main(void) {
  struct timespec request = {.tv_sec=0, .tv_nsec=2000000};
  struct timespec remaining = {.tv_sec=111, .tv_nsec=222};
  struct timespec before, after;
  if (clock_gettime(CLOCK_MONOTONIC, &before)) return 1;
  if (syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &request, &remaining)) return 2;
  if (clock_gettime(CLOCK_MONOTONIC, &after)) return 3;
  if (ns(&after) - ns(&before) < 1000000LL ||
      ns(&after) - ns(&before) > 1000000000LL) return 4;
  if (remaining.tv_sec != 111 || remaining.tv_nsec != 222) return 5;
  if (absolute_sleep(CLOCK_MONOTONIC) || absolute_sleep(CLOCK_REALTIME)) return 6;
  errno = 0;
  if (syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 2, &request, NULL) != -1 ||
      errno != EINVAL) return 7;
  errno = 0;
  if (syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, NULL, NULL) != -1 ||
      errno != EFAULT) return 8;
  errno = 0;
  if (syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, (void*)1, NULL) != -1 ||
      errno != EFAULT) return 9;
  request.tv_sec = -1;
  errno = 0;
  if (syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &request, NULL) != -1 ||
      errno != EINVAL) return 10;
  request.tv_sec = 0;
  request.tv_nsec = 1000000000L;
  errno = 0;
  if (syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &request, NULL) != -1 ||
      errno != EINVAL) return 11;
  request.tv_nsec = 0;
  errno = 0;
  if (syscall(SYS_clock_nanosleep, CLOCK_PROCESS_CPUTIME_ID, 0,
              &request, NULL) != -1 || errno != EOPNOTSUPP) return 12;
  errno = 0;
  if (syscall(SYS_clock_nanosleep, CLOCK_THREAD_CPUTIME_ID, 0,
              &request, NULL) != -1 || errno != EINVAL) return 13;
  if (syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &request,
              (void*)1) != 0) return 14; /* no EINTR means no remainder write */
  puts("GUEST_CLOCK_NANOSLEEP_OK");
  return 0;
}
