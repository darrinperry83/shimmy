/* ARM64 setpgid probe: guest selectors must never become host PID selectors. */
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0")=a, x1 __asm__("x1")=b;
  register long x2 __asm__("x2")=c, x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char regular[]="GUEST_SETPGID_REGULAR_OK\n";
  static const char init[]="GUEST_SETPGID_INIT_OK\n";
  if (!code) {
    const char *msg = sc(172,0,0,0)==1 ? init : regular;
    sc(64,1,(long)msg, msg==init ? sizeof init-1 : sizeof regular-1);
  }
  sc(93,code,0,0);
  for (;;) {}
}
void _start(void) {
  long self=sc(172,0,0,0);
  if (self<=0) done(1);
  if (sc(154,0,-1,0)!=-22) done(2);           // negative group: EINVAL
  if (sc(154,-1,0,0)!=-3) done(3);            // absent process: ESRCH
  if (sc(154,2147483647L,0,0)!=-3) done(4);  // never target a host PID
  if (sc(154,0,2147483647L,0)!=-95) done(5); // cannot join a host group
  if (sc(154,self,2147483647L,0)!=-95) done(6);
  // Both Linux self selectors must map to host (0, 0), including guest PID 1.
  long r=sc(154,0,0,0);
  if (r!=0 && r!=-1) done(7);                 // host may refuse a session leader
  long explicit_self=sc(154,self,self,0);
  if (explicit_self!=r) done(8);
  done(0);
}
