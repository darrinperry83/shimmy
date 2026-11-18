/* ARM64 raw ABI: checked readlinkat path and output, no trailing NUL. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "READLINKAT_CHECKED_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static const char link_path[] __attribute__((section(".text"))) = "/ordinary-link";
static const char target[] = "local-target";
static const char proc_exe[] = "/proc/self/exe";
static const char host_link[] = "/host-link";
static char out[64];
static int equal(const char* a, const char* b, long n) {
  for (long i = 0; i < n; i++) if (a[i] != b[i]) return 0;
  return 1;
}
void _start(void) {
  if (sc4(36, (long)target, -100, (long)link_path, 0) != 0) done(1);
  for (int i = 0; i < 64; i++) out[i] = 'Z';
  if (sc4(78, -100, (long)link_path, (long)out, 64) != 12) done(2);
  if (!equal(out, target, 12) || out[12] != 'Z') done(3);
  for (int i = 0; i < 64; i++) out[i] = 'Z';
  if (sc4(78, -100, (long)link_path, (long)out, 3) != 3) done(4);
  if (!equal(out, "loc", 3) || out[3] != 'Z') done(5);
  if (sc4(78, -100, 1, (long)out, 64) != -14) done(6);
  if (sc4(78, -100, (long)link_path, 1, 64) != -14) done(7);
  if (sc4(78, -100, (long)link_path, (long)out, 0) != -22) done(8);
  for (int i = 0; i < 64; i++) out[i] = 'Z';
  if (sc4(78, -100, (long)proc_exe, (long)out, 64) != 10) done(9);
  if (!equal(out, "/bin/probe", 10) || out[10] != 'Z') done(10);
  for (int i = 0; i < 64; i++) out[i] = 'Z';
  if (sc4(78, -100, (long)host_link, (long)out, 3) != 3) done(11);
  if (!equal(out, "/ta", 3) || out[3] != 'Z') done(12);
  done(0);
}
