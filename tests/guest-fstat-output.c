#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>

_Static_assert(sizeof(struct stat) == 128, "aarch64 Linux stat ABI changed");

int main(void) {
  int fd = open("/tmp/fstat-fixture", O_RDONLY);
  if (fd < 0) return 1;
  struct { struct stat value; uint64_t canary; } out = {
      .canary=0x7a05d18ec3b42f69ULL};
  if (syscall(SYS_fstat, fd, &out.value) != 0 ||
      !S_ISREG(out.value.st_mode) || out.value.st_size != 7 ||
      out.canary != 0x7a05d18ec3b42f69ULL) return 2;
  errno = 0;
  if (syscall(SYS_fstat, fd, NULL) != -1 || errno != EFAULT) return 3;
  errno = 0;
  if (syscall(SYS_fstat, fd, (void *)1) != -1 || errno != EFAULT) return 4;
  errno = 0;
  if (syscall(SYS_fstat, -1, &out.value) != -1 || errno != EBADF) return 5;
  close(fd);
  puts("GUEST_FSTAT_OUTPUT_OK");
  return 0;
}
