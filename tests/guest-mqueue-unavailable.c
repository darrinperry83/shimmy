// ARM64 guest ABI guard: no partial mqueue success before fd dispatch exists.
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_MQUEUE_UNAVAILABLE_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char name[] = "/shimmy-probe";
  // All six Linux ARM64 POSIX mqueue calls currently fail cleanly. This is a
  // guard against advertising a fake open while the other calls remain absent.
  for (long nr = 180; nr <= 185; ++nr)
    if (sc4(nr, (long)name, 0, 0, 0) != -38) done((int)(nr - 179));
  done(0);
}
