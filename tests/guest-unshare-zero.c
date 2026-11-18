#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

int main(void) {
  char cwd_before[512], cwd_after[512];
  if (!getcwd(cwd_before, sizeof cwd_before)) return 1;
  pid_t before = getpid();
  uid_t uid_before = getuid();
  int pipefd[2];
  if (pipe(pipefd)) return 2;
  if (syscall(SYS_unshare, 0) != 0) return 3;
  if (getpid() != before || getuid() != uid_before ||
      !getcwd(cwd_after, sizeof cwd_after) ||
      strcmp(cwd_before, cwd_after)) return 4;
  if (write(pipefd[1], "x", 1) != 1) return 5;
  char byte = 0;
  if (read(pipefd[0], &byte, 1) != 1 || byte != 'x') return 6;
  errno = 0;
  if (syscall(SYS_unshare, CLONE_NEWNS) != -1 || errno != EPERM) return 7;
  close(pipefd[0]); close(pipefd[1]);
  puts("GUEST_UNSHARE_ZERO_OK");
  return 0;
}
