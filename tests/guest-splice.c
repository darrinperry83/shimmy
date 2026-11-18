#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static long raw_splice(long in, long *in_off, long out, long *out_off,
                       long count, long flags) {
  register long x0 __asm__("x0") = in;
  register long x1 __asm__("x1") = (long)in_off;
  register long x2 __asm__("x2") = out;
  register long x3 __asm__("x3") = (long)out_off;
  register long x4 __asm__("x4") = count;
  register long x5 __asm__("x5") = flags;
  register long x8 __asm__("x8") = 76;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}

int main(void) {
  int in = open("/tmp/splice-in", O_CREAT | O_RDWR | O_TRUNC, 0600);
  int out = open("/tmp/splice-out", O_CREAT | O_RDWR | O_TRUNC, 0600);
  int p[2];
  if (in < 0 || out < 0 || pipe(p) || write(in, "abcdef", 6) != 6 ||
      write(out, "______", 6) != 6) return 1;
  long in_off = 2, out_off = 2;
  if (raw_splice(in, &in_off, p[1], NULL, 3, 0) != 3 || in_off != 5) return 2;
  if (lseek(in, 0, SEEK_CUR) != 6) return 3;
  if (raw_splice(p[0], NULL, out, &out_off, 3, 0) != 3 || out_off != 5) return 4;
  if (lseek(out, 0, SEEK_CUR) != 6) return 5;
  char got[7] = {0};
  if (lseek(out, 0, SEEK_SET) != 0 || read(out, got, 6) != 6 ||
      memcmp(got, "__cde_", 6)) return 6;
  if (lseek(in, 0, SEEK_SET) != 0 ||
      raw_splice(in, NULL, p[1], NULL, 2, 0) != 2 ||
      lseek(in, 0, SEEK_CUR) != 2) return 7;
  if (read(p[0], got, 2) != 2 || memcmp(got, "ab", 2)) return 8;
  if (raw_splice(in, (void*)1, p[1], NULL, 1, 0) != -14) return 9;
  if (raw_splice(p[0], &in_off, out, NULL, 1, 0) != -29) return 10;
  if (raw_splice(in, NULL, p[1], &out_off, 1, 0) != -29) return 11;
  if (raw_splice(in, NULL, out, NULL, 1, 0) != -22) return 12;
  if (raw_splice(p[0], NULL, p[1], NULL, 1, 0) != -95) return 13;
  if (raw_splice(in, NULL, p[1], NULL, 1, 0x100) != -22) return 14;
  if (raw_splice(-1, NULL, -1, NULL, 0, 0xff) != 0) return 15;
  if (raw_splice(p[0], NULL, out, NULL, 1, 2) != -11) return 16;
  close(in); close(out); close(p[0]); close(p[1]);
  puts("GUEST_SPLICE_OK");
  return 0;
}
