// libc-free ARM64 guest probe for checked self-process process_vm_readv.
#include <stdint.h>
struct vec { void *base; unsigned long len; };

static long call(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2),
                   "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_PROCESS_VM_READV_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  call(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}
static long readv(long pid, struct vec *local, long lc,
                  struct vec *remote, long rc, long flags) {
  return call(270, pid, (long)local, lc, (long)remote, rc, flags);
}

void _start(void) {
  long self = call(172, 0, 0, 0, 0, 0, 0); // getpid
  if (self <= 0) done(1);
  char src1[] = "abc", src2[] = "def";
  char dst1[] = "xx", dst2[] = "xxxx";
  struct vec local[] = {{dst1, 2}, {dst2, 4}};
  struct vec remote[] = {{src1, 3}, {src2, 3}};
  if (readv(self, local, 2, remote, 2, 0) != 6 ||
      dst1[0] != 'a' || dst1[1] != 'b' ||
      dst2[0] != 'c' || dst2[1] != 'd' ||
      dst2[2] != 'e' || dst2[3] != 'f') done(2);
  if (readv(self, local, 2, remote, 2, 1) != -22) done(3);
  if (readv(0, local, 2, remote, 2, 0) != -3) done(4);
  if (readv(self + 99999, local, 2, remote, 2, 0) != -3) done(5);
  if (readv(self, (struct vec*)1, 1, remote, 2, 0) != -14) done(6);
  dst1[0] = 'z';
  local[1].base = (void*)1;
  if (readv(self, local, 2, remote, 2, 0) != -14 || dst1[0] != 'z') done(7);
  local[1].base = dst2;
  remote[0].len = 2; remote[1].base = (void*)0x100000000L;
  if (readv(self, local, 2, remote, 2, 0) != 2 ||
      dst1[0] != 'a' || dst1[1] != 'b') done(8);
  remote[0].base = dst1; remote[0].len = 2;
  if (readv(self, local, 2, remote, 1, 0) != -95) done(9);
  if (readv(self, local, 17, remote, 1, 0) != -95) done(10);
  if (readv(self, 0, 0, 0, 0, 0) != 0) done(11);
  done(0);
}
