/* Raw ARM64 ABI probe: no guest LSM accepts a security context. */
#include <stdint.h>
static long sc(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0")=a, x1 __asm__("x1")=b;
  register long x2 __asm__("x2")=c, x3 __asm__("x3")=d, x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[]="GUEST_LSM_SET_OK\n";
  if (!code) sc(64,1,(long)ok,sizeof ok-1,0);
  sc(93,code,0,0,0);
  for (;;) {}
}
struct ctx { uint64_t id, flags, len, ctx_len; char payload[16]; };
static struct ctx c={101,0,32,0,{0}};
void _start(void) {
  if (sc(460,100,(long)&c,32,0)!=-95) done(1); // valid, no hook
  if (sc(460,100,(long)&c,32,1)!=-22) done(2);
  if (sc(460,100,(long)&c,31,0)!=-22) done(3);
  if (sc(460,100,(long)&c,16385,0)!=-7) done(4);
  if (sc(460,100,0,32,0)!=-14) done(5);
  if (sc(460,100,1,32,0)!=-14) done(6);
  c.len=33;
  if (sc(460,100,(long)&c,32,0)!=-22) done(7);
  c.len=32; c.ctx_len=1;
  if (sc(460,100,(long)&c,32,0)!=-22) done(8);
  c.len=40; c.ctx_len=8;
  if (sc(460,100,(long)&c,40,0)!=-95) done(9);
  c.ctx_len=(uint64_t)-1;
  if (sc(460,100,(long)&c,40,0)!=-22) done(10);
  done(0);
}
