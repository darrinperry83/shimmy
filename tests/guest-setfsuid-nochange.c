#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
  unsigned current = (unsigned)geteuid();
  if (syscall(SYS_setfsuid, -1) != current) return 1;
  if (syscall(SYS_setfsuid, current) != current) return 2;
  errno = 0;
  if (syscall(SYS_setfsuid, current ^ 1u) != -1 || errno != EOPNOTSUPP)
    return 3;
  if ((unsigned)geteuid() != current || syscall(SYS_setfsuid, -1) != current)
    return 4;
  puts("GUEST_SETFSUID_NOCHANGE_OK");
  return 0;
}
