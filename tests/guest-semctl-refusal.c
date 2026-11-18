/* ARM64 raw ABI: semctl must never touch a host-global semaphore ID. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) static void done(int code) {
  static const char ok[] = "SEMCTL_REFUSAL_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  unsigned short values[2] = {0xa5a5, 0xa5a5};
  if (sc4(191, 0, 0, 0, 0) != -38) done(1);          /* IPC_RMID */
  if (sc4(191, 12345, 0, 12, 0) != -38) done(2);     /* GETVAL */
  if (sc4(191, 12345, 0, 16, 1) != -38) done(3);     /* SETVAL */
  if (sc4(191, 12345, 0, 13, (long)values) != -38) done(4); /* GETALL */
  if (sc4(191, 12345, 0, 17, (long)values) != -38) done(5); /* SETALL */
  if (sc4(191, -1, 0, 2, (long)values) != -38) done(6);       /* IPC_STAT */
  if (sc4(191, 12345, 0, 999, (long)values) != -38) done(7);
  if (values[0] != 0xa5a5 || values[1] != 0xa5a5) done(8);
  done(0);
}
