#include <stdint.h>

static long sc(long nr,long a,long b,long c,long d,long e,long f){
  register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
  register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
  register long x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code){
  if(!code){static const char ok[]="SETSOCKOPT_INPUT_OK\n";sc(64,1,(long)ok,sizeof ok-1,0,0,0);}
  sc(93,code,0,0,0,0,0);for(;;){}
}
void _start(void){
  long fd=sc(198,2,2,0,0,0,0); if(fd<0)finish(1); /* AF_INET SOCK_DGRAM */
  int before=0, after=0, value=65536;
  uint32_t len=sizeof before;
  if(sc(209,fd,1,8,(long)&before,(long)&len,0)!=0 || len!=4)finish(2);
  if(sc(208,fd,1,8,1,4,0)!=-14)finish(3); /* invalid guest optval */
  len=sizeof after;
  if(sc(209,fd,1,8,(long)&after,(long)&len,0)!=0 ||
     len!=4 || after!=before)finish(4); /* no mutation */
  if(sc(208,fd,1,8,0,4,0)!=-14)finish(5);
  if(sc(208,fd,1,8,(long)&value,65537,0)!=-95)finish(6);
  if(sc(208,fd,1,8,(long)&value,4,0)!=0)finish(7);
  len=sizeof after;
  if(sc(209,fd,1,8,(long)&after,(long)&len,0)!=0 || len!=4 || after<=0)finish(8);
  finish(0);
}
