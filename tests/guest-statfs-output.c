/* Disposable ARM64 probe for statfs output bounds and invalid guest memory. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_STATFS_OUTPUT_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char path[] = "/tmp";
  static const char empty[] = "";
  static const char missing[] = "/does-not-exist";
  static struct { long long fields[15]; unsigned long long canary; } out;
  out.canary = 0x9a5bc7d1032246e8ULL;
  if (sc3(43, 0, (long)&out, 0) != -14) done(7);
  if (sc3(43, 1, (long)&out, 0) != -14) done(8);
  if (sc3(43, (long)empty, (long)&out, 0) != -2) done(9);
  if (sc3(43, (long)path, 0, 0) != -14) done(1);
  if (sc3(43, (long)path, 1, 0) != -14) done(2);
  if (sc3(43, (long)missing, (long)&out, 0) != -2) done(3);
  if (sc3(43, (long)path, (long)&out, 0) != 0) done(4);
  if (out.fields[0] != 0xEF53 || out.fields[1] <= 0) done(5);
  if (out.canary != 0x9a5bc7d1032246e8ULL) done(6);
  done(0);
}
