#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

static long raw_preadv(long fd, const struct iovec *vec, long nvec, long pos, long high) {
  register long x0 __asm__("x0") = fd;
  register long x1 __asm__("x1") = (long)vec;
  register long x2 __asm__("x2") = nvec;
  register long x3 __asm__("x3") = pos;
  register long x4 __asm__("x4") = high;
  register long x8 __asm__("x8") = 69;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}

int main(void) {
  int fd = open("/tmp/preadv-probe", O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (fd < 0 || write(fd, "abcdef", 6) != 6) return 1;
  char a[3] = {0}, b[4] = {0};
  struct iovec vec[2] = {{a, 2}, {b, 3}};
  if (raw_preadv(fd, vec, 2, 1, 0x12345678) != 5) return 2;
  if (memcmp(a, "bc", 2) || memcmp(b, "def", 3)) return 3;
  if (lseek(fd, 0, SEEK_CUR) != 6) return 4;
  if (raw_preadv(fd, vec, 2, 5, 0) != 1 || a[0] != 'f') return 5;
  if (raw_preadv(fd, vec, 2, -1, 0) != -22) return 6;
  if (raw_preadv(fd, (void*)1, 2, 0, 0) != -14) return 7;
  if (raw_preadv(fd, vec, 1025, 0, 0) != -22) return 8;
  struct iovec bad = {(void*)1, 1};
  if (raw_preadv(fd, &bad, 1, 0, 0) != -14) return 9;
  if (raw_preadv(fd, NULL, 0, 0, 0) != 0) return 10;
  int pipefd[2];
  if (pipe(pipefd) != 0 || raw_preadv(pipefd[0], vec, 2, 0, 0) != -29) return 11;
  close(pipefd[0]); close(pipefd[1]);
  const long high_pos = 5L * 1024 * 1024 * 1024;
  if (lseek(fd, high_pos, SEEK_SET) != high_pos || write(fd, "Q", 1) != 1) return 12;
  if (raw_preadv(fd, vec, 2, high_pos, 0xffffffff) != 1 || a[0] != 'Q') return 13;
  close(fd);
  if (raw_preadv(fd, vec, 2, 0, 0) != -9) return 14;
  puts("GUEST_PREADV_OK");
  return 0;
}
