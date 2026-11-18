/* AArch64 guest probe: no msgctl command may reach the host SysV namespace. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_MSGCTL_REFUSE_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  /* Linux IPC_RMID=0, IPC_SET=1, IPC_STAT=2. No queue is created or read. */
  unsigned char buffer[120];
  for (unsigned i = 0; i < sizeof buffer; ++i) buffer[i] = 0xa5;
  if (sc3(187, 0, 0, 0) != -38) done(1);
  if (sc3(187, 12345, 0, 0) != -38) done(2);
  if (sc3(187, 12345, 1, (long)buffer) != -38) done(3);
  if (sc3(187, 12345, 2, (long)buffer) != -38) done(4);
  if (sc3(187, -1, 2, (long)buffer) != -38) done(5);
  if (sc3(187, 12345, 999, (long)buffer) != -38) done(6);
  for (unsigned i = 0; i < sizeof buffer; ++i) if (buffer[i] != 0xa5) done(7);
  done(0);
}
