// libc-free ARM64 guest probe for checked self-process process_vm_writev.
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
  static const char ok[] = "GUEST_PROCESS_VM_WRITEV_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  call(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}
static long writev(long pid, struct vec *local, long lc,
                   struct vec *remote, long rc, long flags) {
  return call(271, pid, (long)local, lc, (long)remote, rc, flags);
}

void _start(void) {
  long self = call(172, 0, 0, 0, 0, 0, 0); // getpid
  if (self <= 0) done(1);
  char left[] = "ab", right[] = "cd", dest[] = "xxxxxx";
  struct vec local[] = {{left, 2}, {right, 2}};
  struct vec remote[] = {{dest, 3}, {dest + 3, 1}};
  if (writev(self, local, 2, remote, 2, 0) != 4 ||
      dest[0] != 'a' || dest[1] != 'b' || dest[2] != 'c' ||
      dest[3] != 'd' || dest[4] != 'x') done(2);
  if (writev(self, local, 2, remote, 2, 1) != -22) done(3);
  if (writev(0, local, 2, remote, 2, 0) != -3) done(4);
  if (writev(self + 99999, local, 2, remote, 2, 0) != -3) done(5);
  if (writev(self, (struct vec*)1, 1, remote, 2, 0) != -14) done(6);
  dest[0] = 'z';
  local[1].base = (void*)1;
  if (writev(self, local, 2, remote, 2, 0) != -14 || dest[0] != 'z') done(7);
  local[1].base = right;
  remote[0].len = 2; remote[1].base = (void*)0x100000000L;
  if (writev(self, local, 2, remote, 2, 0) != 2 ||
      dest[0] != 'a' || dest[1] != 'b') done(8);
  remote[0].base = left; remote[0].len = 2;
  if (writev(self, local, 2, remote, 1, 0) != -95) done(9);
  if (writev(self, local, 17, remote, 1, 0) != -95) done(10);
  if (writev(self, 0, 0, 0, 0, 0) != 0) done(11);
  done(0);
}
