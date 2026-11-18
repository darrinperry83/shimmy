/* AArch64 raw sysinfo ABI: checked output without changing synthetic fields. */
#include <stdint.h>
struct linux_sysinfo {
  int64_t uptime;
  uint64_t loads[3];
  uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
  uint16_t procs, pad;
  uint64_t totalhigh, freehigh;
  uint32_t mem_unit;
};
_Static_assert(sizeof(struct linux_sysinfo) == 112, "Linux aarch64 sysinfo size");
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "SYSINFO_COPYOUT_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  struct { struct linux_sysinfo info; uint64_t marker; } out;
  out.marker = 0x1122334455667788ULL;
  if (sc(179, (long)&out.info, 0, 0) != 0) done(1);
  if (out.info.uptime < 0 || !out.info.totalram ||
      out.info.freeram != out.info.totalram / 4 ||
      out.info.mem_unit != 1 || out.info.procs != 1 ||
      out.info.totalswap != 0 || out.info.freeswap != 0) done(2);
  if (out.marker != 0x1122334455667788ULL) done(3);
  if (sc(179, 0, 0, 0) != -14) done(4);
  if (sc(179, 1, 0, 0) != -14) done(5);
  if (sc(179, (long)&out.info, 0, 0) != 0 ||
      out.marker != 0x1122334455667788ULL) done(6);
  done(0);
}
