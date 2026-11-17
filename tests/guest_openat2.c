/* Tiny libc-free aarch64 Linux guest probe for Shimmy syscall 437.
 * Build: clang --target=aarch64-linux-gnu -nostdlib -static-pie -fuse-ld=lld
 *        -Wl,-e,_start tests/guest_openat2.c -o <private-root>/bin/probe
 * Put X in <private-root>/fixture, then run shim_cli <private-root> /bin/probe.
 */
#include <stdint.h>
struct open_how { uint64_t flags, mode, resolve; };
static long call(long nr, long a0, long a1, long a2, long a3) {
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) void _start(void) {
  struct open_how how = { .flags = 0, .mode = 0, .resolve = 0 };
  long fd = call(437, -100, (long)"/fixture", (long)&how, sizeof how);
  if (fd < 0) call(93, 10, 0, 0, 0);
  char b = 0;
  if (call(63, fd, (long)&b, 1, 0) != 1 || b != 'X') call(93, 11, 0, 0, 0);
  call(57, fd, 0, 0, 0);
  how.resolve = 0x8;  /* RESOLVE_BENEATH must never be silently ignored. */
  if (call(437, -100, (long)"/fixture", (long)&how, sizeof how) != -95)
    call(93, 12, 0, 0, 0);
  call(93, 0, 0, 0, 0);
  for (;;) {}
}
