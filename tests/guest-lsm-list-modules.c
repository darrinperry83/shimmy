/* Raw ARM64 probe for the guest's empty Linux Security Module list. */
#include <stdint.h>
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0")=a, x1 __asm__("x1")=b;
  register long x2 __asm__("x2")=c, x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[]="GUEST_LSM_LIST_OK\n";
  if (!code) sc(64,1,(long)ok,sizeof ok-1);
  sc(93,code,0,0);
  for (;;) {}
}
void _start(void) {
  uint32_t size=8;
  uint64_t ids=0xfeedfaceULL;
  if (sc(461,(long)&ids,(long)&size,0)!=0 || size!=0 || ids!=0xfeedfaceULL) done(1);
  size=0;
  if (sc(461,0,(long)&size,0)!=0 || size!=0) done(2);
  size=99;
  if (sc(461,0,(long)&size,1)!=-22 || size!=99) done(3);
  if (sc(461,0,0,0)!=-14) done(4);
  if (sc(461,0,1,0)!=-14) done(5);
  done(0);
}
