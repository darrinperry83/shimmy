/* AArch64 guest probe: msgrcv must not read a Darwin SysV queue by host ID. */
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_MSGRCV_REFUSE_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  struct { long type; char payload[16]; } msg = {0};
  /* Plausible raw host IDs and both blocking and IPC_NOWAIT requests. */
  if (sc5(188, 1, (long)&msg, sizeof msg.payload, 0, 0) != -38) done(1);
  if (sc5(188, 12345, (long)&msg, sizeof msg.payload, 2, 04000) != -38) done(2);
  if (sc5(188, 1, 0, sizeof msg.payload, 0, 04000) != -38) done(3);
  if (msg.type != 0 || msg.payload[0] != 0) done(4);
  done(0);
}
