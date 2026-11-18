// ARM64 raw syslog syscall probe. Shimmy has no virtual printk ring yet.
#include <stdint.h>

static long raw_syslog(long type, void* buffer, long len) {
  register long x0 __asm__("x0") = type;
  register long x1 __asm__("x1") = (long)buffer;
  register long x2 __asm__("x2") = len;
  register long x8 __asm__("x8") = 116;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static long raw_write(long fd, const void* buffer, long len) {
  register long x0 __asm__("x0") = fd;
  register long x1 __asm__("x1") = (long)buffer;
  register long x2 __asm__("x2") = len;
  register long x8 __asm__("x8") = 64;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(long code) {
  static const char ok[] = "GUEST_SYSLOG_OK\n";
  if (!code) raw_write(1, ok, sizeof ok - 1);
  register long x0 __asm__("x0") = code;
  register long x8 __asm__("x8") = 93;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  for (;;) {}
}
void _start(void) {
  char buffer[8] = {'Z', 'Z', 'Z', 'Z', 'Z', 'Z', 'Z', 'Z'};
  if (raw_syslog(3, buffer, sizeof buffer) != -38) finish(1);  // READ_ALL
  if (raw_syslog(10, 0, 0) != -38) finish(2);                 // SIZE_BUFFER
  if (raw_syslog(0, 0, 0) != -1) finish(3);                  // CLOSE, CAP_SYSLOG
  if (raw_syslog(2, buffer, sizeof buffer) != -1) finish(4); // READ, CAP_SYSLOG
  if (raw_syslog(8, 0, 4) != -1) finish(5);                  // CONSOLE_LEVEL
  if (raw_syslog(99, 0, 0) != -1) finish(6);                 // permission precedes type
  for (unsigned i = 0; i < sizeof buffer; ++i)
    if (buffer[i] != 'Z') finish(7);
  finish(0);
}
