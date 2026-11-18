/* ARM64 probe: current-process nice is readable, host namespaces are not. */
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0")=a, x1 __asm__("x1")=b;
  register long x2 __asm__("x2")=c, x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[]="GUEST_GETPRIORITY_SCOPE_OK\n";
  if (!code) sc(64,1,(long)ok,sizeof ok-1);
  sc(93,code,0,0);
  for (;;) {}
}
void _start(void) {
  long pid=sc(172,0,0,0);
  if (pid<=0) done(1);
  long self=sc(141,0,0,0);
  if (self<1 || self>40) done(2); // raw Linux ABI: 20 - nice
  if (sc(141,0,pid,0)!=self) done(3);
  if (sc(141,3,0,0)!=-22) done(4);
  if (sc(141,0,2147483647L,0)!=-3) done(5);
  if (sc(141,1,2147483647L,0)!=-95) done(6);
  if (sc(141,2,2147483647L,0)!=-95) done(7);
  done(0);
}
