/* Raw ARM64 utimensat ABI, translation, and checked-pointer probe. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_UTIMENSAT_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
struct ts { long sec, nsec; };
static struct ts fixed[2] = {{1000000000, 0}, {1200000000, 0}};
static struct ts special[2] = {{0, 0x3ffffffe}, {0, 0x3fffffff}};
void _start(void) {
  static const char path[] = "/stamp";
  static const char link[] = "/link";
  static const char empty[] = "";
  long fd = sc4(56, -100, (long)path, 0x42, 0600); /* O_CREAT|O_RDWR */
  if (fd < 0) finish(1);
  if (sc4(88, -100, (long)path, (long)fixed, 0)) finish(2);
  if (sc4(88, fd, (long)empty, (long)fixed, 0x1000)) finish(3);
  if (sc4(88, fd, 0, (long)fixed, 0)) finish(4);
  if (sc4(88, fd, 0, (long)fixed, 0x8000) != -22) finish(5);
  if (sc4(88, -100, (long)path, 1, 0) != -14) finish(6);
  if (sc4(88, -100, 1, (long)fixed, 0) != -14) finish(7);
  special[1].nsec = 1000000000;
  if (sc4(88, -100, (long)path, (long)special, 0) != -22) finish(8);
  special[1].nsec = 0x3fffffff;
  if (sc4(88, -100, (long)path, (long)special, 0)) finish(9);
  if (sc4(88, fd, (long)empty, (long)fixed, 0) != -2) finish(10);
  if (sc4(88, -100, (long)link, (long)fixed, 0x100)) finish(11);
  if (sc4(88, 999999, 0, (long)fixed, 0) != -9 ||
      sc4(88, 999999, (long)empty, (long)fixed, 0x1000) != -9) finish(12);
  if (sc4(88, -100, 0, (long)fixed, 0) != -14) finish(13);
  if (sc4(88, -100, (long)empty, (long)fixed, 0x1000)) finish(14);
  /* shim_cli reserves its private stderr duplicate at fd 900. It is a host
   * descriptor, not a guest target, even though fcntl can still see it. */
  if (sc4(25, 900, 1, 0, 0) < 0) finish(15);
  if (sc4(88, 900, 0, (long)fixed, 0) != -9 ||
      sc4(88, 900, (long)empty, (long)fixed, 0x1000) != -9) finish(16);
  long opath = sc4(56, -100, (long)path, 0x200000, 0);
  if (opath < 0 || sc4(88, opath, (long)empty, (long)fixed, 0x1000)) finish(17);
  sc4(57, opath, 0, 0, 0);
  long linkpath = sc4(56, -100, (long)link, 0x200000 | 0x8000, 0);
  if (linkpath < 0 ||
      sc4(88, linkpath, (long)empty, (long)fixed, 0x1000)) finish(18);
  sc4(57, linkpath, 0, 0, 0);
  if (sc4(88, -100, (long)path, (long)special, 0)) finish(19);
  sc4(57, fd, 0, 0, 0);
  finish(0);
}
