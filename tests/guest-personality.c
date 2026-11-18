#include <stdint.h>
#include <stdio.h>

static long raw_personality(unsigned int request) {
  register long x0 __asm__("x0") = request;
  register long x8 __asm__("x8") = 92;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}

int main(void) {
  if (raw_personality(UINT32_MAX) != 0) return 1;
  if (raw_personality(0) != 0) return 2;
  if (raw_personality(UINT32_MAX) != 0) return 3;
  if (raw_personality(0x00040000u) != -95) return 4; // ADDR_NO_RANDOMIZE
  if (raw_personality(0x00400000u) != -95) return 5; // READ_IMPLIES_EXEC
  if (raw_personality(8u) != -95) return 6;          // PER_LINUX32
  if (raw_personality(UINT32_MAX) != 0) return 7;   // reject must not mutate
  puts("GUEST_PERSONALITY_OK");
  return 0;
}
