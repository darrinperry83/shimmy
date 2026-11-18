#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

struct output {
  unsigned char statfs[120];
  uint64_t canary;
};

int main(void) {
  int fd = open("/tmp/fstatfs-file", O_RDONLY);
  if (fd < 0) return 1;
  struct output out = {.canary = 0xa6d94872f30be15cULL};
  if (syscall(SYS_fstatfs, fd, out.statfs) != 0 ||
      out.canary != 0xa6d94872f30be15cULL) return 2;
  errno = 0;
  if (syscall(SYS_fstatfs, fd, NULL) != -1 || errno != EFAULT) return 3;
  errno = 0;
  if (syscall(SYS_fstatfs, fd, (void *)1) != -1 || errno != EFAULT) return 4;
  errno = 0;
  if (syscall(SYS_fstatfs, -1, (void *)1) != -1 || errno != EBADF) return 5;

  int pidfd = (int)syscall(SYS_pidfd_open, getpid(), 0);
  if (pidfd < 0) return 6;
  out.canary = 0xa6d94872f30be15cULL;
  if (syscall(SYS_fstatfs, pidfd, out.statfs) != 0 ||
      *(uint64_t *)out.statfs != 0x09041934ULL ||
      out.canary != 0xa6d94872f30be15cULL) return 7;
  errno = 0;
  if (syscall(SYS_fstatfs, pidfd, (void *)1) != -1 || errno != EFAULT) return 8;
  close(pidfd);
  close(fd);
  puts("GUEST_FSTATFS_OUTPUT_OK");
  return 0;
}
