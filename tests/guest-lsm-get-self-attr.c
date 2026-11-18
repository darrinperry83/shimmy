/* Raw ARM64 ABI probe: no guest LSM provides a security context. */
#include <stdint.h>
static long sc(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0")=a, x1 __asm__("x1")=b;
  register long x2 __asm__("x2")=c, x3 __asm__("x3")=d, x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[]="GUEST_LSM_SELF_OK\n";
  if (!code) sc(64,1,(long)ok,sizeof ok-1,0);
  sc(93,code,0,0,0);
  for (;;) {}
}
void _start(void) {
  uint32_t size=32;
  uint64_t id=101;
  if (sc(459,100,(long)&id,(long)&size,0)!=-95 || size!=0 || id!=101) done(1);
  size=32;
  if (sc(459,100,0,(long)&size,0)!=-95 || size!=0) done(2);
  size=32;
  if (sc(459,0,0,(long)&size,0)!=-22 || size!=32) done(3);
  if (sc(459,100,0,0,0)!=-22) done(4);
  if (sc(459,100,0,1,0)!=-14) done(5);
  size=32;
  if (sc(459,100,0,(long)&size,1)!=-22 || size!=32) done(6);
  if (sc(459,100,0,(long)&size,2)!=-22 || size!=32) done(7);
  id=0;
  if (sc(459,100,(long)&id,(long)&size,1)!=-22 || size!=32) done(8);
  id=101;
  if (sc(459,100,(long)&id,(long)&size,1)!=-95 || size!=0 || id!=101) done(9);
  size=32;
  if (sc(459,100,1,(long)&size,1)!=-14 || size!=32) done(10);
  done(0);
}
