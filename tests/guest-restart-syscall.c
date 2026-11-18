// libc-free ARM64 guest probe: no restart block is installed.
static long call(long nr, long a) {
  register long x0 __asm__("x0") = a;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}

void _start(void) {
  static const char ok[] = "GUEST_RESTART_NO_BLOCK_OK\n";
  if (call(128, 0) == -4) {
    register long x0 __asm__("x0") = 1;
    register long x1 __asm__("x1") = (long)ok;
    register long x2 __asm__("x2") = sizeof ok - 1;
    register long x8 __asm__("x8") = 64;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    call(93, 0);
  }
  call(93, 1);
  for (;;) {}
}
