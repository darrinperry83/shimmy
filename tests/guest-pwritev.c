#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

static long raw_pwritev(long fd, const struct iovec *vec, long nvec, long pos, long high) {
  register long x0 __asm__("x0") = fd;
  register long x1 __asm__("x1") = (long)vec;
  register long x2 __asm__("x2") = nvec;
  register long x3 __asm__("x3") = pos;
  register long x4 __asm__("x4") = high;
  register long x8 __asm__("x8") = 70;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}

int main(void) {
  int fd = open("/tmp/pwritev-probe", O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (fd < 0 || write(fd, "abcdef", 6) != 6) return 1;
  char a[] = "XY", b[] = "Z";
  struct iovec vec[2] = {{a, 2}, {b, 1}};
  if (raw_pwritev(fd, vec, 2, 1, 0x12345678) != 3) return 2;
  if (lseek(fd, 0, SEEK_CUR) != 6) return 3;
  char got[7] = {0};
  if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, got, 6) != 6 ||
      memcmp(got, "aXYZef", 6)) return 4;
  if (raw_pwritev(fd, vec, 2, -1, 0) != -22) return 5;
  if (raw_pwritev(fd, (void*)1, 2, 0, 0) != -14) return 6;
  if (raw_pwritev(fd, vec, 1025, 0, 0) != -22) return 7;
  struct iovec bad = {(void*)1, 1};
  if (raw_pwritev(fd, &bad, 1, 0, 0) != -14) return 8;
  struct iovec later_bad[2] = {{a, 2}, {(void*)1, 1}};
  if (raw_pwritev(fd, later_bad, 2, 0, 0) != -14) return 16;
  if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, got, 6) != 6 ||
      memcmp(got, "aXYZef", 6)) return 17;
  if (raw_pwritev(fd, NULL, 0, 0, 0) != 0) return 9;
  int pipefd[2];
  if (pipe(pipefd) != 0 || raw_pwritev(pipefd[1], vec, 2, 0, 0) != -29) return 10;
  close(pipefd[0]); close(pipefd[1]);
  int readonly = open("/tmp/pwritev-probe", O_RDONLY);
  if (readonly < 0 || raw_pwritev(readonly, vec, 2, 0, 0) != -9) return 11;
  close(readonly);
  int append = open("/tmp/pwritev-probe", O_WRONLY | O_APPEND);
  if (append < 0 || raw_pwritev(append, vec, 2, 0, 0) != -95) return 12;
  close(append);
  const long high_pos = 5L * 1024 * 1024 * 1024;
  if (raw_pwritev(fd, vec, 2, high_pos, 0xffffffff) != 3) return 13;
  if (lseek(fd, high_pos, SEEK_SET) != high_pos || read(fd, got, 3) != 3 ||
      memcmp(got, "XYZ", 3)) return 14;
  close(fd);
  if (raw_pwritev(fd, vec, 2, 0, 0) != -9) return 15;
  puts("GUEST_PWRITEV_OK");
  return 0;
}
