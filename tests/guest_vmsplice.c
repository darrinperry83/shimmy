#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void) {
  int pipefd[2];
  if (pipe(pipefd)) return 1;
  char a[] = "shim", b[] = "my";
  struct iovec iov[2] = {{a, 4}, {b, 2}};
  if (syscall(SYS_vmsplice, pipefd[1], iov, 2, 0) != 6) return 2;
  char out[7] = {0};
  if (read(pipefd[0], out, 6) != 6 || strcmp(out, "shimmy")) return 3;
  if (syscall(SYS_vmsplice, pipefd[1], iov, 0, 0) != 0) return 4;
  errno = 0;
  if (syscall(SYS_vmsplice, pipefd[1], iov, 1025, 0) != -1 || errno != EINVAL) return 5;
  errno = 0;
  if (syscall(SYS_vmsplice, pipefd[1], iov, 2, 0x100) != -1 || errno != EINVAL) return 6;
  int regular = open("/tmp/guest-vmsplice-regular", O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (regular < 0) return 7;
  errno = 0;
  if (syscall(SYS_vmsplice, regular, iov, 2, 0) != -1 || errno != EINVAL) return 8;
  close(regular);
  unlink("/tmp/guest-vmsplice-regular");
  errno = 0;
  if (syscall(SYS_vmsplice, pipefd[1], iov, 2, 2) != -1 || errno != EOPNOTSUPP) return 9;
  if (fcntl(pipefd[1], F_SETFL, O_NONBLOCK) < 0) return 10;
  if (syscall(SYS_vmsplice, pipefd[1], iov, 2, 2) != 6) return 11;
  if (read(pipefd[0], out, 6) != 6 || strcmp(out, "shimmy")) return 12;
  close(pipefd[0]); close(pipefd[1]);
  puts("GUEST_VMSPLICE_OK");
  return 0;
}
