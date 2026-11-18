/* ARM64 raw syscall probe: shmctl must not touch host SysV IPC. */
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) static void done(int code) {
  static const char ok[] = "SHMCTL_REFUSAL_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  unsigned char scratch[128]; /* never read: refusal precedes host IPC */
  if (sc(195, 0x5a17, 0, 0) != -38) done(1); /* IPC_RMID */
  if (sc(195, 0x5a17, 2, (long)scratch) != -38) done(2); /* IPC_STAT */
  if (sc(195, 0x5a17, 1, (long)scratch) != -38) done(3); /* IPC_SET */
  done(0);
}
