/* AArch64 static PIE: no host semaphore is opened, created or modified. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <sys/sem.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

int main(void) {
  struct sembuf op = {.sem_num = 0, .sem_op = 1, .sem_flg = 0};
  struct timespec limit = {.tv_sec = 0, .tv_nsec = 1000000};
  errno = 0;
  if (syscall(SYS_semtimedop, 0, &op, 1, &limit) != -1 || errno != ENOSYS)
    return 1;
  errno = 0;
  if (syscall(SYS_semtimedop, INT_MAX, &op, 1, NULL) != -1 || errno != ENOSYS)
    return 2;
  errno = 0;
  if (syscall(SYS_semtimedop, INT_MAX, (void *)1, 1, (void *)1) != -1 ||
      errno != ENOSYS) return 3;
  if (op.sem_op != 1 || limit.tv_nsec != 1000000) return 4;
  puts("SEMTIMEDOP_REFUSED_PASS");
  return 0;
}
