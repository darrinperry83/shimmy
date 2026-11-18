/* AArch64 static PIE: no host SysV queue is opened, created, or sent to. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

struct message { long type; char text[8]; };

int main(void) {
  struct message msg = {1, "persist"};
  errno = 0;
  if (syscall(SYS_msgsnd, 0, &msg, 0, 0) != -1 || errno != ENOSYS) return 1;
  errno = 0;
  if (syscall(SYS_msgsnd, INT_MAX, &msg, sizeof msg.text, 0) != -1 ||
      errno != ENOSYS) return 2;
  errno = 0;
  if (syscall(SYS_msgsnd, INT_MAX, (void *)1, sizeof msg.text, 0) != -1 ||
      errno != ENOSYS) return 3;
  if (msg.text[0] != 'p') return 4;
  puts("MSGSND_REFUSED_PASS");
  return 0;
}
