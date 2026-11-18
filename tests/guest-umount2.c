/* libc-free ARM64 probe: umount2 must never claim a synthetic detach. */
#include <stdint.h>

static long call(long nr,long a,long b,long c){
  register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
  register long x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) static void done(int code){call(93,code,0,0);for(;;){}}
void _start(void){
  static const char mnt[]="/mnt", missing[]="/missing", empty[]="";
  for(int flags=0;flags<=8;flags=flags ? flags*2 : 1){
    if(call(39,(long)mnt,flags,0)!=-1)done(1+flags);
  }
  if(call(39,(long)mnt,16,0)!=-22)done(11);
  if(call(39,0,0,0)!=-14)done(12);
  if(call(39,0x30000,0,0)!=-14)done(13);
  if(call(39,(long)empty,0,0)!=-2)done(14);
  if(call(39,(long)missing,0,0)!=-2)done(15);
  static const char message[]="UMOUNT2_REFUSAL_OK\n";
  call(64,1,(long)message,sizeof(message)-1);
  done(0);
}
