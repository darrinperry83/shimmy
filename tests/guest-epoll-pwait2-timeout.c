#include <stdint.h>

static long sc(long nr,long a,long b,long c,long d,long e,long f){
  register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
  register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
  register long x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code){
  if(!code){static const char ok[]="EPOLL_PWAIT2_TIMEOUT_OK\n";sc(64,1,(long)ok,sizeof ok-1,0,0,0);}
  sc(93,code,0,0,0,0,0);for(;;){}
}
struct event {uint32_t events,pad;uint64_t data;};
struct timespec64 {int64_t sec,nsec;};
void _start(void){
  long ep=sc(20,0,0,0,0,0,0); if(ep<0)finish(1);
  int fds[2]={-1,-1}; if(sc(59,(long)fds,0,0,0,0,0))finish(2);
  struct event interest={1,0,UINT64_C(0x123456789abcdef0)};
  if(sc(21,ep,1,fds[0],(long)&interest,0,0))finish(3);
  static const char one='x';
  if(sc(64,fds[1],(long)&one,1,0,0,0)!=1)finish(4);
  struct event out={0,0,UINT64_C(0x5555555555555555)};
  if(sc(441,ep,(long)&out,1,1,0,0)!=-14)finish(5); /* bad timeout */
  if(out.data!=UINT64_C(0x5555555555555555))finish(6);
  struct timespec64 timeout={-1,0};
  if(sc(441,ep,(long)&out,1,(long)&timeout,0,0)!=-22)finish(7);
  timeout.sec=0; timeout.nsec=1000000000;
  if(sc(441,ep,(long)&out,1,(long)&timeout,0,0)!=-22)finish(8);
  timeout.nsec=0;
  if(sc(441,ep,(long)&out,1,(long)&timeout,0,0)!=1)finish(9);
  if(!(out.events&1) || out.pad || out.data!=interest.data)finish(10);
  finish(0);
}
