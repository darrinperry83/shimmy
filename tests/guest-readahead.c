/* libc-free ARM64 probe for checked readahead on a regular guest file. */
static long call(long nr,long a,long b,long c,long d){
  register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
  register long x3 __asm__("x3")=d,x8 __asm__("x8")=nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) static void done(int code){call(93,code,0,0,0);for(;;){}}
__attribute__((noreturn)) void _start(void){
  static const char file[]="/bin/sample";
  static const char dir[]="/bin";
  long fd=call(56,-100,(long)file,0,0);
  if(fd<0)done(10);
  if(call(213,fd,0,4,0)!=0)done(11);
  if(call(213,fd,0,0,0)!=0)done(12); /* zero count: through EOF */
  if(call(213,fd,4096,4,0)!=0)done(13); /* after EOF */
  if(call(213,fd,-1,4,0)!=-22)done(14);
  if(call(213,-1,0,4,0)!=-9)done(15);
  if(call(62,fd,0,1,0)!=0)done(16); /* advice did not advance file offset */
  long writefd=call(56,-100,(long)file,1,0);
  if(writefd<0 || call(213,writefd,0,4,0)!=-9)done(17);
  long dirfd=call(56,-100,(long)dir,0,0);
  if(dirfd<0 || call(213,dirfd,0,4,0)!=-22)done(18);
  done(0);
}
