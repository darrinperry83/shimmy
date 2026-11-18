/* libc-free aarch64 probe for the bounded process_madvise ABI. */
#include <stdint.h>

static long call(long nr,long a,long b,long c,long d,long e,long f){
  register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
  register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
  register long x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) static void done(int code){call(93,code,0,0,0,0,0);for(;;){}}
struct iov { uint64_t base,len; };
__attribute__((noreturn)) void _start(void){
  long pid=call(172,0,0,0,0,0,0);
  long pfd=call(434,pid,0,0,0,0,0);
  if(pfd<0)done(10);
  long page=call(222,0,16384,3,0x22,-1,0);
  if(page<0)done(11);
  struct iov one={(uint64_t)page,16384};
  if(call(440,pfd,(long)&one,1,0,0,0)!=16384)done(12);
  one.len=19;
  if(call(440,pfd,(long)&one,1,0,0,0)!=19)done(13);
  if(call(440,pfd,(long)&one,1,0,1,0)!=-22)done(14);
  if(call(440,-1,(long)&one,1,0,0,0)!=-9)done(15);
  if(call(440,pfd,0,1,0,0,0)!=-14)done(16);
  if(call(440,pfd,(long)&one,1,4,0,0)!=-95)done(17);
  one.base=(uint64_t)page+1;
  if(call(440,pfd,(long)&one,1,0,0,0)!=-22)done(18);
  one.base=0x30000;
  if(call(440,pfd,(long)&one,1,0,0,0)!=-12)done(19);
  one.base=(uint64_t)page;
  if(call(440,pfd,(long)&one,2,0,0,0)!=-95)done(20);
  if(call(440,pfd,0,0,0,0,0)!=0)done(21);
  static const char message[]="PROCESS_MADVISE_OK\n";
  call(64,1,(long)message,sizeof(message)-1,0,0,0);
  done(0);
}
