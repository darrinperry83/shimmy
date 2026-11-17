/* Run under shim_cli to exercise the guest's raw AArch64 syscall ABI. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct sched_attr_v0 {
  uint32_t size, policy;
  uint64_t flags;
  int32_t nice;
  uint32_t priority;
  uint64_t runtime, deadline, period;
};

static long call4(long nr, long a0, long a1, long a2, long a3) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
  return x0;
}

int main(void) {
  unsigned char bytes[64];
  memset(bytes, 0xa5, sizeof bytes);
  if (call4(275, 0, (long)bytes, sizeof bytes, 0)) return 1;
  const struct sched_attr_v0 *a = (const void *)bytes;
  if (a->size != sizeof *a || a->policy != 0 || a->flags ||
      a->nice < -20 || a->nice > 19 || a->priority ||
      a->runtime || a->deadline || a->period) return 2;
  for (size_t i = sizeof *a; i < sizeof bytes; ++i)
    if (bytes[i]) return 3;
  long tid = call4(178, 0, 0, 0, 0);
  if (tid <= 0 || call4(275, tid, (long)bytes, 48, 0)) return 4;
  if (call4(275, 0, (long)bytes, 47, 0) != -22 ||
      call4(275, 0, (long)bytes, 4097, 0) != -22 ||
      call4(275, 0, 0, 48, 0) != -22 ||
      call4(275, -1, (long)bytes, 48, 0) != -22 ||
      call4(275, 0, (long)bytes, 48, 1) != -22) return 5;
  if (call4(275, 2000000000, (long)bytes, 48, 0) != -3) return 6;
  puts("sched_getattr guest ABI PASS");
  return 0;
}
