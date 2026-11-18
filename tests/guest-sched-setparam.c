/* libc-free ARM64 probe for the bounded sched_setparam ABI. */
#include <stdint.h>
static long call(long nr,long a,long b,long c){
  register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
  register long x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) static void done(int code){call(93,code,0,0);for(;;){}}
__attribute__((noreturn)) void _start(void){
  int zero=0, one=1;
  long tid=call(178,0,0,0), pid=call(172,0,0,0);
  if(tid<=0 || pid<=0)done(10);
  if(call(118,0,(long)&zero,0)!=0)done(11);
  if(call(118,tid,(long)&zero,0)!=0)done(12);
  if(call(118,pid,(long)&zero,0)!=0)done(13);
  if(call(118,0,(long)&one,0)!=-22)done(14);
  if(call(118,-1,(long)&zero,0)!=-22)done(15);
  if(call(118,0,0,0)!=-22)done(16);
  if(call(118,0,0x30000,0)!=-14)done(17);
  if(call(118,0x7fffffff,(long)&zero,0)!=-3)done(18);
  static const char msg[]="SCHED_SETPARAM_OK\n";
  call(64,1,(long)msg,sizeof(msg)-1);done(0);
}
