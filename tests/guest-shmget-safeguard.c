/* ARM64 raw ABI: only non-creating keys are probed, with no host IPC effect. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_SHMGET_SAFEGUARD_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  /* Neither call sets IPC_CREAT. The historical route would only look up. */
  if (sc3(194, 0x4671, 4096, 0) != -38) finish(1);
  if (sc3(194, 0x4672, 0, 0) != -38) finish(2);
  finish(0);
}
