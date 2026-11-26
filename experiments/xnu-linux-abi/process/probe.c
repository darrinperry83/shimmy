/* Unchanged Linux ELF: native COW fork, TLS, waitid and signal termination. */
typedef unsigned long u64;
static long call(u64 n,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f){
 register u64 x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f,x8 __asm__("x8")=n;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");return (long)x0;
}
#define SC(n,a,b,c,d) call(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d),0,0)
#define CHECK(e) do{if(!(e)){static const char message[]="PROCESS_CHECK " #e "\n";SC(64,2,message,sizeof(message)-1,0);return stage;}}while(0)
struct info {int signo,error,code,pad;int pid;unsigned uid;int status,pad2;long utime,stime;u64 spare[10];};
_Static_assert(sizeof(struct info)==128,"siginfo size");
static u64 tls[8];
static int valid_tls(void){u64 p,c;__asm__ volatile("mrs %0,tpidr_el0\nmov %1,x18":"=r"(p),"=r"(c));return p==(u64)tls&&c==0x137;}
static volatile u64 shared=0x1234;
static long waitid(int kind,long pid,struct info *out,int flags){return call(95,kind,pid,(u64)out,flags,0,0);}
long probe_main(u64 *initial){
 int stage=130;CHECK(initial[0]==4);
 const char *loader=(const char *)initial[2],*libraries=(const char *)initial[3],*program=(const char *)initial[4];
 long parent=SC(172,0,0,0,0),uid=SC(174,0,0,0,0);
 CHECK(parent>1 && SC(173,0,0,0,0)>0);
 __asm__ volatile("msr tpidr_el0,%0\nmov x18,#0x137"::"r"(tls):"memory");
 u64 old_mask=0,block=1UL<<11;CHECK(SC(135,0,&block,&old_mask,8)==0);
 u64 base=SC(214,0,0,0,0);CHECK(base && SC(214,base+16384,0,0,0)==(long)(base+16384));
 volatile u64 *heap=(volatile u64 *)base;heap[0]=0x5678;
 struct info event;
 CHECK(waitid(0,0,&event,4)==-10 && waitid(9,0,&event,4)==-22);
 CHECK(waitid(1,0,&event,4)==-22 && waitid(0,0,&event,0)==-22);
 long pid=SC(220,17,0,0,0);CHECK(pid>=0 && valid_tls());
 if(pid==0){
  u64 child_mask=0;CHECK(SC(135,0,0,&child_mask,8)==0 && (child_mask&block));
  stage=131;CHECK(SC(172,0,0,0,0)!=parent && SC(173,0,0,0,0)==parent);
  CHECK(shared==0x1234 && heap[0]==0x5678);
  shared=0xabcd;heap[0]=0xef01;
  CHECK(SC(214,0,0,0,0)==(long)(base+16384));
  CHECK(SC(214,base+32768,0,0,0)==(long)(base+32768));
  static const char ready[]="CHILD_READY\n";CHECK(SC(64,1,ready,sizeof(ready)-1,0)==sizeof(ready)-1);
  char c=0;CHECK(SC(63,3,&c,1,0)==1 && c=='T' && valid_tls());
  return 0x123425; /* Linux must expose only 0x25 (37). */
 }
 stage=132;CHECK(pid!=parent && shared==0x1234 && heap[0]==0x5678);
 CHECK(SC(214,0,0,0,0)==(long)(base+16384));
 unsigned char *bytes=(unsigned char *)&event;for(unsigned i=0;i<sizeof(event);i++)bytes[i]=0xa5;
 CHECK(waitid(1,pid,&event,5)==0 && event.signo==0 && event.pid==0);
 static const char waiting[]="PARENT_WAIT\n";CHECK(SC(64,1,waiting,sizeof(waiting)-1,0)==sizeof(waiting)-1);
 stage=133;
 CHECK(waitid(1,pid,&event,4|0x1000000)==0 && valid_tls());
 CHECK(event.signo==17 && event.code==1 && event.pid==pid && event.uid==(unsigned)uid && event.status==37);
 CHECK(waitid(1,pid,(struct info *)1,4|0x1000000)==-14);
 CHECK(waitid(1,pid,&event,5|0x1000000)==0 && event.status==37 && event.pid==pid);
 CHECK(waitid(0,0,&event,4)==0 && event.pid==pid && event.status==37);
 CHECK(waitid(1,pid,&event,5)==-10 && shared==0x1234 && heap[0]==0x5678);
 stage=134;pid=SC(220,17,0,0,0);CHECK(pid>=0 && valid_tls());
 if(pid==0){for(;;)__asm__ volatile("yield");}
 CHECK(SC(129,pid,0,0,0)==0 && SC(129,pid,10,0,0)==0);
 CHECK(waitid(1,pid,&event,4)==0 && event.signo==17 && event.pid==pid && event.code==2 && event.status==10);
 CHECK(waitid(1,pid,&event,5)==-10 && SC(129,pid,0,0,0)==-3);
 stage=135;pid=SC(220,17,0,0,0);CHECK(pid>=0 && valid_tls());
 if(pid==0){
  const char *args[]={loader,"--inhibit-cache","--library-path",libraries,program,"ARCH_CHILD_EXEC_PASS\\n",0},*env[]={0};
  CHECK(SC(221,"/rdxnu-no-such-executable",args,env,0)==-2 && valid_tls());
  CHECK(SC(221,loader,1,env,0)==-14 && valid_tls());
  SC(221,loader,args,env,0);CHECK(0);
 }
 CHECK(waitid(1,pid,&event,4)==0 && event.pid==pid && event.code==1 && event.status==0);
 CHECK(shared==0x1234 && heap[0]==0x5678 && SC(214,0,0,0,0)==(long)(base+16384));
 CHECK(SC(135,2,&old_mask,0,8)==0);
 CHECK(valid_tls() && SC(214,base,0,0,0)==(long)base);
 static const char pass[]="LINUX_PROCESS_PASS\n";CHECK(SC(64,1,pass,sizeof(pass)-1,0)==sizeof(pass)-1);return 0;
}
__asm__(".text\n.global _start\n_start:\nmov x0,sp\nbl probe_main\nmov x8,#94\nsvc #0\nb .\n");
