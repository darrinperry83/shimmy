/* ARM64 raw ABI: statx snapshots path and checks its 256-byte guest output. */
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "STATX_MEMORY_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
static const char path[] __attribute__((section(".text"))) = "/tmp/data";
static const char absent[] __attribute__((section(".text"))) = "/tmp/absent";
static const char empty[] __attribute__((section(".text"))) = "";
static const char cgroup[] __attribute__((section(".text"))) = "/sys/fs/cgroup";
static const char readonly_out[] __attribute__((section(".text"))) = "R";
static unsigned long output[32]; /* Linux statx: 256 bytes */
#define STATX(dir, name, flags, out) sc5(291, (dir), (name), (flags), 0x7ff, (out))
static unsigned short mode(void) { return *(unsigned short *)((char *)output + 28); }
static long long size(void) { return *(long long *)((char *)output + 40); }
void _start(void) {
  if (STATX(-100, (long)path, 0, (long)output) != 0 ||
      (mode() & 0170000) != 0100000 || size() != 3) done(1);
  ((char *)output)[0] = '?';
  if (STATX(-100, 1, 0, (long)output) != -14 || ((char *)output)[0] != '?') done(2);
  if (STATX(-100, (long)absent, 0, (long)output) != -2 ||
      ((char *)output)[0] != '?') done(3);
  if (STATX(-100, (long)path, 0, 1) != -14) done(4);
  if (STATX(-100, (long)path, 0, (long)readonly_out) != -14 ||
      readonly_out[0] != 'R') done(5);
  long p = sc5(222, 0, 32768, 3, 0x22, -1);
  if (p < 0 || sc5(226, p + 16384, 16384, 0, 0, 0) != 0) done(11);
  ((char *)p)[16384 - 128] = '?';
  if (STATX(-100, (long)path, 0, p + 16384 - 128) != -14 ||
      ((char *)p)[16384 - 128] != '?') done(12);
  if (STATX(-100, 0, 0, (long)output) != -14) done(6);
  long fd = sc5(56, -100, (long)path, 0, 0, 0);
  if (fd < 0) done(7);
  if (STATX(fd, (long)empty, 0x1000, (long)output) != 0 || size() != 3) done(8);
  if (STATX(fd, 0, 0x1000, (long)output) != 0 || size() != 3) done(9);
  if (STATX(-100, (long)cgroup, 0, (long)output) != 0 ||
      (mode() & 0170000) != 0040000) done(10);
  done(0);
}
