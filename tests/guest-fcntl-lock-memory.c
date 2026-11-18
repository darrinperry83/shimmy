/* ARM64 raw ABI: fcntl lock structures are guest memory, not host pointers. */
#include <stdint.h>
struct flock_guest {
  int16_t type, whence;
  int32_t pad;
  int64_t start, len;
  int32_t pid, pad2;
};
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static long sc6(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "FCNTL_LOCK_MEMORY_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static const char path[] __attribute__((section(".text"))) = "/tmp/file";
static const struct flock_guest read_only_lock __attribute__((section(".text"))) =
    {.type = 1, .whence = 0, .start = 0, .len = 1};
static struct flock_guest query = {.type = 1, .whence = 0, .start = 0, .len = 1};
void _start(void) {
  long fd = sc4(56, -100, (long)path, 2, 0); // openat O_RDWR
  if (fd < 0) done(1);
  if (sc4(25, fd, 6, 1, 0) != -14) done(2); // SETLK bad pointer
  if (sc4(25, fd, 5, 1, 0) != -14) done(3); // GETLK bad pointer
  if (sc4(25, fd, 5, (long)&read_only_lock, 0) != -14) done(4);
  long p = sc6(222, 0, 32768, 3, 0x22, -1, 0);
  if (p < 0 || sc4(226, p + 16384, 16384, 0, 0) != 0) done(5);
  for (int i = 0; i < 16; ++i) ((char *)(p + 16384 - 16))[i] = ((const char *)&read_only_lock)[i];
  if (sc4(25, fd, 6, p + 16384 - 16, 0) != -14) done(6); // later bytes protected
  if (sc4(25, fd, 6, (long)&read_only_lock, 0) != 0) done(7);
  if (sc4(25, fd, 5, (long)&query, 0) != 0 || query.type != 2) done(8);
  if (sc4(57, fd, 0, 0, 0) != 0) done(9);
  done(0);
}
