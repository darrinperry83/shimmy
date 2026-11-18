#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static long raw_syncfs(long fd) {
  register long x0 __asm__("x0") = fd;
  register long x8 __asm__("x8") = 267;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}

int main(void) {
  int fd = open("/tmp/syncfs-probe", O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (fd < 0) return 1;
  if (write(fd, "x", 1) != 1) return 2;
  if (raw_syncfs(fd) != -38) return 3;       /* valid: no false success */
  int dir = open("/tmp", O_RDONLY | O_DIRECTORY);
  if (dir < 0 || raw_syncfs(dir) != -38) return 4;
  close(fd);
  if (raw_syncfs(fd) != -9) return 5;        /* EBADF */
  if (raw_syncfs(-1) != -9) return 6;
  close(dir);
  puts("GUEST_SYNCFS_ABI_OK");
  return 0;
}
