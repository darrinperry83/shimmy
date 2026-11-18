/* libc-free ARM64 Linux guest probe for bounded madvise behavior. */
#include <stdint.h>

static long call(long nr,long a,long b,long c,long d,long e,long f){
  register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
  register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
  register long x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) static void done(int code){call(93,code,0,0,0,0,0);for(;;){}}
void _start(void){
  long page=call(222,0,16384,3,0x22,-1,0); /* anonymous private */
  if(page<0)done(1);
  *(volatile uint64_t *)page=0xdeadbeefUL;
  for(int advice=0;advice<=3;advice++){
    if(call(233,page,1,advice,0,0,0)!=0)done(2+advice);
    if(*(volatile uint64_t *)page!=0xdeadbeefUL)done(6);
  }
  if(call(233,page+1,1,0,0,0,0)!=-22)done(7);
  if(call(233,page,1,4,0,0,0)!=-95)done(8); /* DONTNEED */
  if(*(volatile uint64_t *)page!=0xdeadbeefUL)done(9);
  if(call(233,page,1,8,0,0,0)!=-95)done(10); /* FREE */
  if(call(233,page,1,102,0,0,0)!=-95)done(11); /* GUARD_INSTALL */
  if(call(233,page,1,999,0,0,0)!=-22)done(12);
  if(call(233,page,-1,0,0,0,0)!=-22)done(13);
  if(call(233,0x30000,16384,0,0,0,0)!=-12)done(14);
  if(call(233,page,0,0,0,0,0)!=0)done(15);
  if(call(215,page,16384,0,0,0,0)!=0)done(16);
  if(call(233,page,1,0,0,0,0)!=-12)done(17);
  static const char message[]="MADVISE_BOUNDED_OK\n";
  call(64,1,(long)message,sizeof(message)-1,0,0,0);
  done(0);
}
