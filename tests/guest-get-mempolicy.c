/* libc-free arm64 guest probe for the bounded one-node get_mempolicy ABI. */
#include <stdint.h>

static long call(long nr,long a,long b,long c,long d,long e){
  register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
  register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) static void done(int code){call(93,code,0,0,0,0);for(;;){}}
__attribute__((noreturn)) void _start(void){
  int policy=77;
  unsigned long mask[3]={~0UL,~0UL,0x12345678UL};
  if(call(236,(long)&policy,(long)mask,66,0,0)!=0)done(10);
  if(policy!=0 || mask[0]!=0 || mask[1]!=0 || mask[2]!=0x12345678UL)done(11);
  policy=77; mask[0]=0; mask[1]=~0UL;
  if(call(236,(long)&policy,(long)mask,66,0,4)!=0)done(12);
  if(policy!=0 || mask[0]!=1 || mask[1]!=0 || mask[2]!=0x12345678UL)done(13);
  mask[0]=0x55;
  if(call(236,0,(long)mask,1,0,4)!=0 || mask[0]!=0x55)done(14);
  if(call(236,0,(long)mask,0,0,4)!=-22)done(15);
  if(call(236,0,(long)mask,32770,0,4)!=-22)done(16);
  if(call(236,0,0,0,0,8)!=-22)done(17);
  if(call(236,0,0,0,0x1234,0)!=-22)done(18);
  if(call(236,0,0,0,0,2)!=-95)done(19);
  if(call(236,0,0,0,0,1)!=-22)done(20);
  if(call(236,0,0,0,0,5)!=-22)done(21);
  if(call(236,0,0x30000,2,0,0)!=-14)done(22);
  if(call(236,0x30000,0,0,0,0)!=-14)done(23);
  static const char message[]="GET_MEMPOLICY_OK\n";
  call(64,1,(long)message,sizeof(message)-1,0,0);
  done(0);
}
