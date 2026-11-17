/* A tiny libc-free arm64 Linux guest probe for move_pages query-only ABI. */
#include <stdint.h>

static long call(long nr,long a,long b,long c,long d,long e,long f){
  register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
  register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
  register long x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) static void done(int code){call(93,code,0,0,0,0,0);for(;;){}}
__attribute__((noreturn)) void _start(void){
  long page=call(222,0,16384,3,0x22,-1,0); /* mmap private anonymous */
  if(page<0)done(10);
  *(volatile char *)page=42; /* resident on the single virtual NUMA node */
  void *pages[2]={(void *)page,(void *)0x30000};
  int status[2]={99,99};
  if(call(239,0,2,(long)pages,0,(long)status,0)!=0)done(11);
  if(status[0]!=0||status[1]!=-14)done(12);
  long untouched=call(222,0,16384,3,0x22,-1,0);
  if(untouched<0)done(17);
  pages[1]=(void *)untouched;
  if(call(239,0,2,(long)pages,0,(long)status,0)!=0 ||
     status[0]!=0 || status[1]!=-2)done(18); /* nonresident: ENOENT */
  int node=0;
  if(call(239,0,1,(long)pages,(long)&node,(long)status,0)!=-95)done(13);
  if(call(239,0,1,(long)pages,0,(long)status,4)!=-22)done(14);
  if(call(239,0,1,0,0,(long)status,0)!=-14)done(15);
  long pid=call(172,0,0,0,0,0,0);
  if(call(239,pid,1,(long)pages,0,(long)status,0)!=0||status[0]!=0)done(16);
  static const char message[]="MOVE_PAGES_QUERY_OK\n";
  call(64,1,(long)message,sizeof(message)-1,0,0,0);
  done(0);
}
