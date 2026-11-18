#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/timex.h>
#include <unistd.h>

/* Check the guest's actual glibc/Linux declaration, not a copy of Shimmy's. */
_Static_assert(sizeof(struct timex) == 208, "ARM64 Linux timex size");
_Static_assert(offsetof(struct timex, time) == 72, "ARM64 Linux time offset");
_Static_assert(offsetof(struct timex, tick) == 88, "ARM64 Linux tick offset");
_Static_assert(offsetof(struct timex, tai) == 160, "ARM64 Linux TAI offset");

int main(void) {
  struct timex tx = {0};
  errno = 0;
  if (syscall(SYS_adjtimex, (void *)1) != -1 || errno != EFAULT) return 1;

  tx.modes = ADJ_OFFSET;
  tx.offset = 123456;
  errno = 0;
  if (syscall(SYS_adjtimex, &tx) != -1 || errno != EOPNOTSUPP ||
      tx.modes != ADJ_OFFSET || tx.offset != 123456) return 2;

  memset(&tx, 0xa5, sizeof tx);
  tx.modes = 0;
  long state = syscall(SYS_adjtimex, &tx);
  if (state < TIME_OK || state > TIME_ERROR) return 3;
  if (tx.time.tv_sec < 1700000000 || tx.time.tv_usec < 0 ||
      tx.time.tv_usec >= ((tx.status & STA_NANO) ? 1000000000 : 1000000))
    return 4;
  if (tx.tick != 10000 || tx.precision <= 0 || tx.tolerance <= 0) return 5;
  if (tx.modes != 0) return 6;
  puts("GUEST_ADJTIMEX_UAPI_OK");
  return 0;
}
