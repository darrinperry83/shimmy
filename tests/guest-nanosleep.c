#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long long ns(const struct timespec *t) {
  return (long long)t->tv_sec * 1000000000LL + t->tv_nsec;
}
int main(void) {
  struct timespec request = {.tv_sec=0, .tv_nsec=2000000};
  struct timespec remaining = {.tv_sec=111, .tv_nsec=222};
  struct timespec before, after;
  if (clock_gettime(CLOCK_MONOTONIC, &before)) return 1;
  if (syscall(SYS_nanosleep, &request, &remaining)) return 2;
  if (clock_gettime(CLOCK_MONOTONIC, &after)) return 3;
  if (ns(&after) - ns(&before) < 1000000LL ||
      ns(&after) - ns(&before) > 1000000000LL) return 4;
  if (remaining.tv_sec != 111 || remaining.tv_nsec != 222) return 5;
  errno = 0;
  if (syscall(SYS_nanosleep, NULL, NULL) != -1 || errno != EFAULT) return 6;
  errno = 0;
  if (syscall(SYS_nanosleep, (void*)1, NULL) != -1 || errno != EFAULT) return 7;
  request.tv_sec = -1; request.tv_nsec = 0;
  errno = 0;
  if (syscall(SYS_nanosleep, &request, NULL) != -1 || errno != EINVAL) return 8;
  request.tv_sec = 0; request.tv_nsec = 1000000000L;
  errno = 0;
  if (syscall(SYS_nanosleep, &request, NULL) != -1 || errno != EINVAL) return 9;
  request.tv_nsec = -1;
  errno = 0;
  if (syscall(SYS_nanosleep, &request, NULL) != -1 || errno != EINVAL) return 10;
  request.tv_nsec = 0;
  if (syscall(SYS_nanosleep, &request, (void*)1) != 0) return 11;
  puts("GUEST_NANOSLEEP_OK");
  return 0;
}
