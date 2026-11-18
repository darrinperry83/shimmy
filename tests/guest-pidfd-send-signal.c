#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif

static long send_signal(int fd, int sig, void *info, unsigned flags) {
  return syscall(SYS_pidfd_send_signal, fd, sig, info, flags);
}

int main(void) {
  int pfd = (int)syscall(SYS_pidfd_open, getpid(), 0);
  if (pfd < 0) { perror("pidfd_open"); return 1; }
  if (send_signal(pfd, 0, 0, 0) != 0) { perror("self signal 0"); return 2; }
  errno = 0;
  if (send_signal(-1, 0, 0, 0) != -1 || errno != EBADF) return 3;
  int ordinary = open("/dev/null", O_RDONLY);
  if (ordinary < 0) return 19;
  errno = 0;
  if (send_signal(ordinary, 0, 0, 0) != -1 || errno != EBADF) return 20;
  close(ordinary);
  errno = 0;
  if (send_signal(pfd, 0, 0, 1) != -1 || errno != EINVAL) return 4;
  int payload = 0;
  errno = 0;
  if (send_signal(pfd, 0, &payload, 0) != -1 || errno != EOPNOTSUPP) return 5;
  errno = 0;
  if (send_signal(pfd, SIGUSR1, 0, 0) != -1 || errno != EOPNOTSUPP) return 6;
  errno = 0;
  if (send_signal(pfd, -1, 0, 0) != -1 || errno != EINVAL) return 7;
  errno = 0;
  if (send_signal(pfd, 64, 0, 0) != -1 || errno != EINVAL) return 21;
  errno = 0;
  if (send_signal(pfd, 35, 0, 0) != -1 || errno != EOPNOTSUPP) return 15;
  int alias = dup(pfd);
  if (alias < 0 || close(pfd) != 0) return 16;
  errno = 0;
  if (send_signal(pfd, 0, 0, 0) != -1 || errno != EBADF) return 17;
  if (send_signal(alias, 0, 0, 0) != 0) return 18;
  int gate[2];
  if (pipe(gate) != 0) return 8;
  pid_t child = fork();
  if (child < 0) return 9;
  if (child == 0) {
    close(gate[1]);
    char c;
    if (read(gate[0], &c, 1) != 1) _exit(10);
    _exit(0);
  }
  close(gate[0]);
  int child_fd = (int)syscall(SYS_pidfd_open, child, 0);
  if (child_fd < 0 || send_signal(child_fd, 0, 0, 0) != 0) return 11;
  if (write(gate[1], "x", 1) != 1) return 12;
  close(gate[1]);
  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 13;
  errno = 0;
  if (send_signal(child_fd, 0, 0, 0) != -1 || errno != ESRCH) return 14;
  puts("GUEST_PIDFD_SIGNAL0_VALIDATED_OK");
  return 0;
}
