/* Raw ARM64 ABI probe: one virtual NUMA node, no per-VMA policy. */
static long sc(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0")=a, x1 __asm__("x1")=b, x2 __asm__("x2")=c;
  register long x3 __asm__("x3")=d, x4 __asm__("x4")=e, x5 __asm__("x5")=f;
  register long x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[]="GUEST_HOME_NODE_OK\n";
  if (!code) sc(64,1,(long)ok,sizeof ok-1,0,0,0);
  sc(93,code,0,0,0,0,0);
  for (;;) {}
}
void _start(void) {
  long page=sc(222,0,16384,3,0x22,-1,0);
  if (page<0) done(1);
  if (sc(450,page,0,0,0,0,0)!=0) done(2);
  if (sc(450,page,1,0,0,0,0)!=-2) done(3); // mapped, no VMA policy
  if (sc(450,0x100000000ul,16384,0,0,0,0)!=-2) done(4); // no VMA
  if (sc(450,page+1,0,0,0,0,0)!=-22) done(5);
  if (sc(450,page,0,0,1,0,0)!=-22) done(6);
  if (sc(450,page,0,1,0,0,0)!=-22) done(7);
  if (sc(450,page,-1L,0,0,0,0)!=-22) done(8);
  if (sc(450,-16384L,32768,0,0,0,0)!=-22) done(9);
  if (sc(215,page,16384,0,0,0,0)!=0) done(10);
  done(0);
}
