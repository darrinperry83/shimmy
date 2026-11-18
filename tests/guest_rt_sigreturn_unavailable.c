/* Compile as an AArch64 static PIE and run in an isolated Shimmy root. */
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
  errno = 0;
  if (syscall(SYS_rt_sigreturn) != -1 || errno != ENOSYS)
    return 1;
  puts("RT_SIGRETURN_UNAVAILABLE_PASS");
  return 0;
}
