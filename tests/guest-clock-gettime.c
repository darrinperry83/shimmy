#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int check_clock(clockid_t clock) {
  struct { struct timespec value; uint64_t canary; } out = {
      .value={.tv_sec=-1, .tv_nsec=-1}, .canary=0x17e55e3ecf6a729bULL};
  if (syscall(SYS_clock_gettime, clock, &out.value) != 0) return 1;
  if (out.value.tv_sec < 0 || out.value.tv_nsec < 0 ||
      out.value.tv_nsec >= 1000000000L ||
      out.canary != 0x17e55e3ecf6a729bULL) return 2;
  return 0;
}
int main(void) {
  if (check_clock(CLOCK_REALTIME) || check_clock(CLOCK_MONOTONIC) ||
      check_clock(CLOCK_BOOTTIME) ||
      check_clock(CLOCK_PROCESS_CPUTIME_ID) ||
      check_clock(CLOCK_THREAD_CPUTIME_ID)) return 10;
  errno = 0;
  if (syscall(SYS_clock_gettime, -123456, NULL) != -1 || errno != EINVAL) return 11;
  errno = 0;
  if (syscall(SYS_clock_gettime, CLOCK_REALTIME, NULL) != -1 || errno != EFAULT) return 12;
  errno = 0;
  if (syscall(SYS_clock_gettime, CLOCK_REALTIME, (void*)1) != -1 ||
      errno != EFAULT) return 13;
  puts("GUEST_CLOCK_GETTIME_OK");
  return 0;
}
