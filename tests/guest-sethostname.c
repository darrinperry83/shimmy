/* Raw ARM64 UTS nodename probe. Built as no-group, shared, and exec verifier. */
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static long sc3(long nr, long a, long b, long c) { return sc5(nr, a, b, c, 0, 0); }
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_SETHOSTNAME_OK\n";
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
static int proc_hostname_eq(const char *expected) {
  static const char path[] = "/proc/sys/kernel/hostname";
  char value[66];
  long fd = sc5(56, -100, (long)path, 0, 0, 0); /* openat */
  if (fd < 0) return (int)fd;
  long size = sc3(63, fd, (long)value, sizeof value); /* read */
  long closed = sc3(57, fd, 0, 0);
  if (closed || size < 1 || size > 65) return 0;
  long i = 0;
  while (expected[i] && i < size && value[i] == expected[i]) ++i;
  return !expected[i] && i + 1 == size && value[i] == '\n';
}
void _start(void) {
  if (sc3(160, (long)&info, 0, 0)) finish(1);
#if defined(VERIFY_EXEC)
  if (!eq(info.nodename, "sibling")) finish(3);
  int proc = proc_hostname_eq("sibling");
  if (proc != 1) finish(proc < 0 ? 20 - proc : 17);
  finish(0);
#elif defined(NO_GROUP)
  if (!eq(info.nodename, "localhost") || !proc_hostname_eq("localhost")) finish(1);
  if (sc3(161, (long)"blocked", 7, 0) != -95) finish(2);
  finish(0);
#elif defined(NON_ROOT)
  if (!eq(info.nodename, "localhost") || !proc_hostname_eq("localhost")) finish(1);
  if (sc3(146, 1000, 0, 0)) finish(15); /* virtual setuid */
  if (sc3(161, (long)"blocked", 7, 0) != -1) finish(16);
  finish(0);
#else
  if (!eq(info.nodename, "localhost") || !proc_hostname_eq("localhost")) finish(1);
  if (sc3(161, (long)"chosen", 6, 0)) finish(4);
  if (sc3(160, (long)&info, 0, 0) || !eq(info.nodename, "chosen") ||
      !proc_hostname_eq("chosen")) finish(5);
  if (sc3(161, 1, 3, 0) != -14) finish(6);
  if (sc3(161, (long)"too-long", 65, 0) != -22) finish(7);
  if (sc3(160, 1, 0, 0) != -14) finish(8);
  long child = sc5(220, 17, 0, 0, 0, 0); /* clone(SIGCHLD) */
  if (child == 0) {
    if (sc3(160, (long)&info, 0, 0) || !eq(info.nodename, "chosen") ||
        !proc_hostname_eq("chosen")) finish(9);
    if (sc3(161, (long)"sibling", 7, 0)) finish(10);
    sc3(93, 0, 0, 0);
    for (;;) {}
  }
  if (child < 0) finish(11);
  int status = -1;
  if (sc5(260, child, (long)&status, 0, 0, 0) != child || status) finish(12);
  if (sc3(160, (long)&info, 0, 0) || !eq(info.nodename, "sibling") ||
      !proc_hostname_eq("sibling")) finish(13);
  static const char path[] = "/bin/verify";
  static char *const argv[] = { (char *)path, 0 };
  /* Host controls these paths. /dev/null is deliberately not a directory: an
   * unfiltered override makes either uname or synthetic /proc fail in verifier. */
  static char *const envp[] = {
    (char *)"RDSOCKETGROUP=/dev/null", (char *)"RDX_MEMFD_DIR=/dev/null", 0
  };
  long result = sc3(221, (long)path, (long)argv, (long)envp);
  (void)result;
  finish(14);
#endif
}
