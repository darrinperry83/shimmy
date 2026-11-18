#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

static long raw_vmsplice(long fd, const struct iovec *vec, long nvec, long flags) {
  register long x0 __asm__("x0") = fd;
  register long x1 __asm__("x1") = (long)vec;
  register long x2 __asm__("x2") = nvec;
  register long x3 __asm__("x3") = flags;
  register long x8 __asm__("x8") = 75;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}

int main(void) {
  int p[2];
  if (pipe(p)) return 1;
  char a[] = "shim", b[] = "my";
  struct iovec vec[2] = {{a, 4}, {b, 2}};
  if (raw_vmsplice(p[1], vec, 2, 0) != 6) return 2;
  char out[7] = {0};
  if (read(p[0], out, 6) != 6 || strcmp(out, "shimmy")) return 3;
  if (raw_vmsplice(p[1], vec, 0, 0) != 0) return 4;
  if (raw_vmsplice(p[1], vec, 1025, 0) != -22) return 5;
  if (raw_vmsplice(p[1], vec, 2, 0x100) != -22) return 6;
  if (raw_vmsplice(p[1], (void*)1, 2, 0) != -14) return 7;
  struct iovec later_bad[2] = {{a, 4}, {(void*)1, 1}};
  if (raw_vmsplice(p[1], later_bad, 2, 0) != -14) return 8;
  if (fcntl(p[0], F_SETFL, O_NONBLOCK) < 0) return 9;
  errno = 0;
  if (read(p[0], out, 1) != -1 || errno != EAGAIN) return 10;
  if (raw_vmsplice(p[1], vec, 2, 2) != -95) return 11;
  if (fcntl(p[1], F_SETFL, O_NONBLOCK) < 0 ||
      raw_vmsplice(p[1], vec, 2, 2) != 6) return 12;
  if (read(p[0], out, 6) != 6 || strcmp(out, "shimmy")) return 13;
  int regular = open("/tmp/vmsplice-checked", O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (regular < 0 || raw_vmsplice(regular, vec, 2, 0) != -22) return 14;
  close(regular); close(p[0]); close(p[1]);
  puts("GUEST_VMSPLICE_CHECKED_OK");
  return 0;
}
