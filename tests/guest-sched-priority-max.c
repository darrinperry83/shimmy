/* libc-free ARM64 probe for Linux sched_get_priority_max policy ranges. */
static long call(long nr,long policy){
  register long x0 __asm__("x0")=policy,x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) static void done(int code){
  register long x0 __asm__("x0")=code,x8 __asm__("x8")=93;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  for(;;){}
}
__attribute__((noreturn)) void _start(void){
  if(call(125,0)!=0)done(10);
  if(call(125,1)!=99)done(11);
  if(call(125,2)!=99)done(12);
  if(call(125,3)!=0)done(13);
  if(call(125,5)!=0)done(14);
  if(call(125,6)!=0)done(15);
  if(call(125,7)!=0)done(16);
  if(call(125,4)!=-22)done(17);
  if(call(125,-1)!=-22)done(18);
  done(0);
}
