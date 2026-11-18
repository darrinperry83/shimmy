// libc-free ARM64 guest probe for bounded copy_file_range semantics.
#include <stdint.h>

static long call(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2),
                   "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_COPY_FILE_RANGE_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  call(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}
static long copy(long in, long* inoff, long out, long* outoff,
                 long count, long flags) {
  return call(285, in, (long)inoff, out, (long)outoff, count, flags);
}
static int equal(const char* a, const char* b, int count) {
  for (int i = 0; i < count; ++i) if (a[i] != b[i]) return 0;
  return 1;
}

void _start(void) {
  static const char srcpath[] = "/tmp/src", dstpath[] = "/tmp/dst";
  static const char data[] = "abcdefghijklmnopqrstuvwxyz";
  long src = call(56, -100, (long)srcpath, 0x242, 0600, 0, 0); // O_CREAT|O_TRUNC|O_RDWR
  long dst = call(56, -100, (long)dstpath, 0x242, 0600, 0, 0);
  if (src < 0 || dst < 0) done(1);
  if (call(64, src, (long)data, sizeof data - 1, 0, 0, 0) != sizeof data - 1)
    done(2);
  long inoff = 2, outoff = 4;
  if (copy(src, &inoff, dst, &outoff, 5, 0) != 5 || inoff != 7 || outoff != 9)
    done(3);
  if (call(62, src, 0, 1, 0, 0, 0) != 26 ||
      call(62, dst, 0, 1, 0, 0, 0) != 0) done(4); // explicit offsets unchanged
  char buf[32] = {0};
  if (call(67, dst, (long)buf, 5, 4, 0, 0) != 5 || !equal(buf, "cdefg", 5))
    done(5);
  inoff = 24; outoff = 10;
  if (copy(src, &inoff, dst, &outoff, 5, 0) != 2 ||
      inoff != 26 || outoff != 12) done(21); // short source copy
  if (call(62, src, 0, 0, 0, 0, 0) != 0 ||
      call(62, dst, 0, 0, 0, 0, 0) != 0) done(6);
  if (copy(src, 0, dst, 0, 3, 0) != 3 ||
      call(62, src, 0, 1, 0, 0, 0) != 3 ||
      call(62, dst, 0, 1, 0, 0, 0) != 3) done(7);
  if (call(67, dst, (long)buf, 3, 0, 0, 0) != 3 || !equal(buf, "abc", 3))
    done(8);
  if (copy(src, &inoff, dst, &outoff, 1, 1) != -22) done(9);
  if (copy(src, (long*)1, dst, &outoff, 1, 0) != -14) done(10);
  inoff = -1;
  if (copy(src, &inoff, dst, &outoff, 1, 0) != -22) done(11);
  inoff = 0; outoff = 1;
  if (copy(src, &inoff, src, &outoff, 5, 0) != -22) done(12);
  outoff = 20;
  if (copy(src, &inoff, src, &outoff, 3, 0) != 3 ||
      inoff != 3 || outoff != 23) done(13);
  if (copy(src, 0, src, 0, 0, 0) != 0) done(17);
  if (copy(-1, 0, dst, 0, 1, 0) != -9) done(14);
  if (copy(src, 0, dst, 0, 1024 * 1024 + 1, 0) != -95) done(15);
  static const char dirpath[] = "/tmp";
  long dir = call(56, -100, (long)dirpath, 0, 0, 0, 0);
  if (dir < 0 || copy(dir, 0, dst, 0, 1, 0) != -21) done(18);
  long append = call(56, -100, (long)dstpath, 0x401, 0, 0, 0);
  if (append < 0 || copy(src, 0, append, 0, 1, 0) != -9) done(19);
  inoff = 0x7fffffffffffffffL;
  if (copy(src, &inoff, dst, 0, 1, 0) != -75) done(20);
  int pipefd[2] = {-1, -1};
  if (call(59, (long)pipefd, 0, 0, 0, 0, 0) != 0 ||
      copy(pipefd[0], 0, dst, 0, 1, 0) != -22) done(16);
  done(0);
}
