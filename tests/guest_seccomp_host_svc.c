/* Read-only negative security probe: can a guest issue a Darwin syscall? */
#include <stdio.h>

int main(void) {
#if !defined(__aarch64__)
#error This probe requires AArch64
#endif
  register long result __asm__("x0") = 0;
  register long darwin_nr __asm__("x16") = 20; /* Darwin getpid */
  register long linux_nr __asm__("x8") = 9999; /* would be ENOSYS if routed */
  __asm__ volatile("svc #0x80" : "+r"(result) : "r"(darwin_nr), "r"(linux_nr) : "memory");
  printf("HOST_SVC_RESULT %ld\n", result);
  return result > 0 ? 0 : 1;
}
