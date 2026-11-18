/* AArch64 raw ABI: stack, static data and executable ELF payloads. */
struct iovec { void* base; unsigned long len; };
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "WRITEV_MEMORY_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static char static_data[] = "D";
static const char jit_text[] __attribute__((section(".text"))) = "J";
static char oversized[1024 * 1024 + 1];
void _start(void) {
  char stack_data[] = "S";
  struct iovec all[] = {{stack_data, 1}, {static_data, 1}, {(void*)jit_text, 1}};
  if (sc3(66, 1, (long)all, 3) != 3) done(1);
  if (sc3(66, 1, 1, 1) != -14) done(2); /* invalid vector list */
  struct iovec bad[] = {{stack_data, 1}, {(void*)1, 1}};
  if (sc3(66, 1, (long)bad, 2) != -14) done(3); /* no partial host write */
  if (sc3(66, 1, 0, 1025) != -22) done(4); /* Linux UIO_MAXIOV */
  struct iovec large[] = {{oversized, sizeof oversized}};
  if (sc3(66, 1, (long)large, 1) != -95) done(5); /* no truncated record */
  done(0);
}
