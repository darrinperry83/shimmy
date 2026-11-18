/* AArch64 guest probe: msgget must not touch Darwin's global IPC namespace. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_MSGGET_REFUSE_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  /* Named key lookup and creation, then IPC_PRIVATE creation. No host queue is made. */
  if (sc3(186, 0x53484d59, 0, 0) != -38) done(1);
  if (sc3(186, 0x53484d59, 01000 | 0600, 0) != -38) done(2);
  if (sc3(186, 0, 0600, 0) != -38) done(3);
  done(0);
}
