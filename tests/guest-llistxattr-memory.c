/* ARM64 raw ABI: llistxattr lists the link inode with checked guest memory. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "LLISTXATTR_MEMORY_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static const char path[] __attribute__((section(".text"))) = "/tmp/link";
static const char readonly_text[] __attribute__((section(".text"))) = "R";
static const char link_name[] = "user.shimmy-link";
static const char target_name[] = "user.shimmy-target";
static char static_out[256];
static int same(const char *a, const char *b, long n) {
  for (long i = 0; i < n; i++) if (a[i] != b[i]) return 0;
  return 1;
}
static int contains(const char *list, long n, const char *name) {
  for (long i = 0; i < n;) {
    long j = 0;
    while (i + j < n && name[j] && list[i + j] == name[j]) j++;
    if (!name[j] && i + j < n && !list[i + j]) return 1;
    while (i < n && list[i++]) {}
  }
  return 0;
}
void _start(void) {
  long n = sc3(12, (long)path, 0, 0);
  if (n <= 0 || n > 255) done(1);
  char stack_out[256];
  stack_out[n] = '?'; static_out[n] = '?';
  if (sc3(12, (long)path, (long)stack_out, sizeof stack_out) != n) done(2);
  if (sc3(12, (long)path, (long)static_out, sizeof static_out) != n) done(3);
  if (!same(stack_out, static_out, n) || stack_out[n] != '?' || static_out[n] != '?')
    done(4);
  if (!contains(stack_out, n, link_name) || contains(stack_out, n, target_name)) done(5);
  long followed = sc3(11, (long)path, (long)static_out, sizeof static_out);
  if (followed <= 0 || !contains(static_out, followed, target_name) ||
      contains(static_out, followed, link_name)) done(6);
  stack_out[0] = '?';
  if (sc3(12, (long)path, (long)stack_out, 1) != -34 || stack_out[0] != '?') done(7);
  if (sc3(12, (long)path, 1, sizeof stack_out) != -14) done(8);
  if (sc3(12, (long)path, (long)readonly_text, sizeof stack_out) != -14 ||
      readonly_text[0] != 'R') done(9);
  if (sc3(12, 1, (long)static_out, sizeof static_out) != -14) done(10);
  if (sc3(12, (long)path, (long)static_out, 1024 * 1024 + 1) != n ||
      !contains(static_out, n, link_name)) done(11);
  done(0);
}
