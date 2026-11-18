/* ARM64 probe: self renice works, host namespaces stay out of reach. */
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0")=a, x1 __asm__("x1")=b;
  register long x2 __asm__("x2")=c, x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[]="GUEST_SETPRIORITY_SCOPE_OK\n";
  if (!code) sc(64,1,(long)ok,sizeof ok-1);
  sc(93,code,0,0);
  for (;;) {}
}
void _start(void) {
  long pid=sc(172,0,0,0);
  if (pid<=0) done(1);
  long original=sc(141,0,0,0); // Linux raw ABI: 20 - nice
  if (original<1 || original>40) done(2);
  if (sc(140,3,0,19)!=-22) done(3);
  if (sc(140,0,2147483647L,19)!=-3) done(4);
  if (sc(140,1,2147483647L,19)!=-95) done(5);
  if (sc(140,2,2147483647L,19)!=-95) done(6);
  long self_result=sc(140,0,pid,19);
  if (self_result!=0 && self_result!=-1) done(7);
  if (sc(141,0,0,0)!=(self_result==0 ? 1 : original)) done(8);
  long clamped_result=sc(140,0,0,100);
  if (clamped_result!=0 && clamped_result!=-1) done(9);
  if (sc(141,0,0,0)!=((self_result==0 || clamped_result==0) ? 1 : original)) done(10);
  done(0);
}
