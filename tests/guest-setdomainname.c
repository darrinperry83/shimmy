/* Raw ARM64 virtual UTS domainname probe. */
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static long sc3(long nr, long a, long b, long c) { return sc5(nr, a, b, c, 0, 0); }
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_SETDOMAINNAME_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
struct uts { char sysname[65], nodename[65], release[65], version[65], machine[65], domain[65]; };
static struct uts info;
static int eq(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}
static int proc_domain_eq(const char *expected) {
  static const char path[] = "/proc/sys/kernel/domainname";
  char value[66];
  long fd = sc5(56, -100, (long)path, 0, 0, 0); /* openat */
  if (fd < 0) return 0;
  long size = sc3(63, fd, (long)value, sizeof value); /* read */
  long closed = sc3(57, fd, 0, 0);
  if (closed || size < 1 || size > 65) return 0;
  long i = 0;
  while (expected[i] && i < size && value[i] == expected[i]) ++i;
  return !expected[i] && i + 1 == size && value[i] == '\n';
}
static int uts_eq(const char *domain) {
  return sc3(160, (long)&info, 0, 0) == 0 &&
         eq(info.nodename, "localhost") && eq(info.domain, domain) &&
         proc_domain_eq(domain);
}
void _start(void) {
#if defined(VERIFY_EXEC)
  if (!uts_eq("child.test")) finish(3);
  finish(0);
#elif defined(NO_GROUP)
  if (!uts_eq("(none)")) finish(1);
  if (sc3(162, (long)"blocked", 7, 0) != -95) finish(2);
  finish(0);
#elif defined(NON_ROOT)
  if (!uts_eq("(none)")) finish(1);
  if (sc3(146, 1000, 0, 0)) finish(15); /* virtual setuid */
  if (sc3(162, (long)"blocked", 7, 0) != -1) finish(16);
  finish(0);
#else
  if (!uts_eq("(none)")) finish(1);
  if (sc3(162, (long)"example.test", 12, 0) || !uts_eq("example.test")) finish(4);
  if (sc3(162, 0, 0, 0) || !uts_eq("")) finish(5);
  if (sc3(162, (long)"example.test", 12, 0) || !uts_eq("example.test")) finish(6);
  if (sc3(162, 1, 3, 0) != -14) finish(7);
  if (sc3(162, (long)"too-long", 65, 0) != -22) finish(8);
  static char max_name[65];
  for (int i = 0; i < 64; ++i) max_name[i] = 'x';
  if (sc3(162, (long)max_name, 64, 0) || !uts_eq(max_name)) finish(18);
  if (sc3(162, (long)"example.test", 12, 0) || !uts_eq("example.test")) finish(19);
  long child = sc5(220, 17, 0, 0, 0, 0); /* clone(SIGCHLD) */
  if (child == 0) {
    if (!uts_eq("example.test")) finish(9);
    if (sc3(162, (long)"child.test", 10, 0)) finish(10);
    sc3(93, 0, 0, 0);
    for (;;) {}
  }
  if (child < 0) finish(11);
  int status = -1;
  if (sc5(260, child, (long)&status, 0, 0, 0) != child || status) finish(12);
  if (!uts_eq("child.test")) finish(13);
  static const char path[] = "/bin/verify";
  static char *const argv[] = { (char *)path, 0 };
  static char *const envp[] = {
    (char *)"RDSOCKETGROUP=/dev/null", (char *)"RDX_MEMFD_DIR=/dev/null", 0
  };
  sc3(221, (long)path, (long)argv, (long)envp);
  finish(14);
#endif
}
