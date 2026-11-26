/* Unchanged Linux AArch64 ELF: epoll/timerfd/signalfd service-loop primitives. */
typedef unsigned long u64;
static long call(u64 n,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f){
 register u64 x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f,x8 __asm__("x8")=n;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");return (long)x0;
}
#define SC(n,a,b,c,d) call(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d),0,0)
#define CHECK(e) do{if(!(e)){static const char msg[]="REACTOR_CHECK " #e "\n";SC(64,2,msg,sizeof(msg)-1,0);return 113;}}while(0)
#define SAY(s) do{static const char msg[]=s;CHECK(SC(64,1,msg,sizeof(msg)-1,0)==sizeof(msg)-1);}while(0)
struct event {unsigned events,pad;u64 data;};
struct time {long sec,nsec;};struct timer {struct time interval,value;};
struct info{int signo,error,code,pad,pid;unsigned uid;int status,pad2;long times[2];u64 spare[10];};
struct signal_record {unsigned signo;int error,code;unsigned pid,uid;int fd;unsigned tid,band,overrun,trapno;int status,value;u64 pointer,utime,stime,addr;unsigned short addr_lsb,pad;int syscall;u64 call_addr;unsigned arch;char spare[28];};
_Static_assert(sizeof(struct signal_record)==128,"signalfd layout");
static int eq(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static long number(const char *s){long n=0;while(*s)n=n*10+*s++-'0';return n;}
static void digits(char *s,long n){char tmp[24];int k=0;do{tmp[k++]=(char)('0'+n%10);n/=10;}while(n);for(int i=0;i<k;i++)s[i]=tmp[k-1-i];s[k]=0;}
static long ctl(int ep,int op,int fd,unsigned bits,u64 data){struct event e={bits,0,data};return SC(21,ep,op,fd,&e);}
static long wait(int ep,struct event *events,int n,int ms){return SC(22,ep,events,n,ms);}
static long writeval(int fd,u64 n){return SC(64,fd,&n,8,0);}
static long reap(long pid,int status){struct info e={0};long n=SC(95,1,pid,&e,4);return n?n:(e.pid==pid&&e.code==1&&e.status==status?0:-1);}
static long closefd(long fd){return SC(57,fd,0,0,0);}
static long delay(long ns){struct time t={ns/1000000000,ns%1000000000};return SC(73,0,0,&t,0);}
static long constraints(void){
 struct event e={1,0,1};long ep=SC(20,0,0,0,0),fd=SC(19,0,0x800,0,0);CHECK(ep>=0&&fd>=0);
 CHECK(ctl(ep,1,fd,0x80000001U,1)==-95);
 CHECK(ctl(ep,1,fd,1,1)==0);long alias=SC(25,fd,0,20,0);CHECK(alias>=20&&closefd(fd)==-95);
 CHECK(closefd(alias)==0 && closefd(fd)==0);
 struct time t={0,0};u64 mask=0;CHECK(call(22,ep,(u64)&e,1,0,(u64)&mask,8)==-95);
 CHECK(wait(ep,&e,33,0)==-95);
 struct {int fd;short events,revents;} poll={ep,1,0};CHECK(SC(73,&poll,1,&t,0)==-95);
 long timer=SC(85,0,0,0,0);CHECK(timer==-95);CHECK(closefd(ep)==0);
 SAY("XNU_REACTOR_CONSTRAINTS_PASS\n");return 0;
}
long probe_main(u64 *stack){
 if(stack[0]==2&&eq((const char *)stack[2],"constraints"))return constraints();
 if(stack[0]==4&&eq((const char *)stack[2],"worker")){
  long fd=number((const char *)stack[3]),ep=number((const char *)stack[4]);struct event event;
  CHECK(wait(ep,&event,1,0)==-9);SAY("REACTOR_CHILD_READY\n");
  char token=0;CHECK(SC(63,3,&token,1,0)==1&&token=='T');CHECK(writeval(fd,9)==8);return 0;
 }
 CHECK(stack[0]==1);const char *self=(const char *)stack[1];struct event events[4];u64 value=0;
 unsigned word=7;struct time zero={0,0},short_wait={0,10000000},bad_time={0,1000000000},before,after;
 CHECK(SC(98,&word,128,6,0)==-11&&SC(98,(char *)&word+1,128,7,0)==-22);
 CHECK(SC(98,4,128,7,0)==-14&&SC(98,&word,128,7,1)==-14);
 CHECK(SC(98,&word,128,7,&bad_time)==-22&&SC(98,&word,129,-1,0)==0);
 CHECK(SC(98,&word,129,0,0)==0&&SC(98,&word,129,0x7fffffff,0)==0);
 CHECK(SC(98,&word,128,7,&zero)==-110&&SC(113,1,&before,0,0)==0);
 CHECK(SC(98,&word,128,7,&short_wait)==-110&&SC(113,1,&after,0,0)==0);
 long elapsed=(after.sec-before.sec)*1000000000+after.nsec-before.nsec;CHECK(elapsed>=8000000&&elapsed<5000000000L);
 long futex_child=SC(220,17,0,0,0);CHECK(futex_child>=0);
 if(!futex_child){word=8;CHECK(SC(98,&word,128,8,&short_wait)==-110);return 21;}
 CHECK(SC(98,&word,129,0x7fffffff,0)==0&&reap(futex_child,21)==0&&word==7);
 SAY("LINUX_PRIVATE_FUTEX_PASS\n");
 CHECK(SC(20,1,0,0,0)==-22);long ep=SC(20,0x80000,0,0,0),fd=SC(19,0,0x800,0,0);CHECK(ep>=0&&fd>=0);
 CHECK(SC(25,ep,1,0,0)==1 && SC(25,ep,3,0,0)==2);
 CHECK(ctl(ep,1,fd,5,0xfedcba9876543210UL)==0 && ctl(ep,1,fd,1,0)==-17);
 CHECK(wait(ep,events,4,0)==1&&events[0].events==4&&events[0].data==0xfedcba9876543210UL);
 CHECK(writeval(fd,2)==8 && wait(ep,events,4,0)==1&&events[0].events==5);
 CHECK(wait(ep,events,1,0)==1&&events[0].events==5);
 CHECK(ctl(ep,3,fd,1,0x123456789abcdef0UL)==0 && SC(21,ep,3,fd,1)==-14);
 CHECK(wait(ep,(void *)1,4,0)==-14 && wait(ep,events,4,0)==1&&events[0].data==0x123456789abcdef0UL);
 CHECK(SC(63,fd,&value,8,0)==8&&value==2 && wait(ep,events,4,0)==0);
 CHECK(SC(21,ep,2,fd,1)==0 && ctl(ep,3,fd,1,0)==-2 && ctl(ep,2,fd,0,0)==-2);
 CHECK(ctl(ep,1,ep,1,0)==-22 && wait(-1,events,1,0)==-9 && wait(ep,events,0,0)==-22);
 CHECK(ctl(ep,1,fd,0x40000005U,0x991)==0);
 CHECK(wait(ep,events,4,0)==1&&events[0].events==4&&wait(ep,events,4,0)==0);
 CHECK(writeval(fd,1)==8&&wait(ep,events,4,0)==0);
 CHECK(ctl(ep,3,fd,0x40000005U,0x992)==0&&wait(ep,events,4,0)==1&&events[0].events==5&&events[0].data==0x992);
 CHECK(wait(ep,events,4,0)==0&&SC(63,fd,&value,8,0)==8&&value==1);
 CHECK(ctl(ep,2,fd,0,0)==0);
 CHECK(ctl(ep,1,fd,1,3)==0 && closefd(fd)==0);
 long reused=SC(19,0,0x800,0,0);CHECK(reused==fd && ctl(ep,3,reused,1,9)==-2 && wait(ep,events,4,0)==0);
 long other=SC(19,1,0x800,0,0);CHECK(other>=0&&writeval(reused,1)==8);
 CHECK(ctl(ep,1,reused,1,44)==0&&ctl(ep,1,other,1,44)==0);
 CHECK(wait(ep,events,4,0)==2 && events[0].data==44&&events[1].data==44);
 CHECK(ctl(ep,3,reused,1,11)==0&&ctl(ep,3,other,1,22)==0);
 CHECK(wait(ep,events,1,0)==1);u64 first=events[0].data;
 CHECK(wait(ep,events,1,0)==1&&events[0].data!=first);
 CHECK(ctl(ep,2,reused,0,0)==0&&closefd(reused)==0&&ctl(ep,2,other,0,0)==0&&closefd(other)==0);
 fd=SC(19,0,0x800,0,0);CHECK(fd>=0&&ctl(ep,1,fd,1,77)==0);
 char a[24],b[24];digits(a,fd);digits(b,ep);long child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){const char *args[]={self,"worker",a,b,0},*env[]={0};SC(221,self,args,env,0);return 114;}
 SAY("REACTOR_EPOLL_WAIT\n");CHECK(wait(ep,events,4,-1)==1&&events[0].data==77&&events[0].events==1);
 CHECK(SC(63,fd,&value,8,0)==8&&value==9&&reap(child,0)==0);
 CHECK(ctl(ep,2,fd,0,0)==0&&closefd(fd)==0);SAY("LINUX_EPOLL_PASS\n");
 /* Native scheduled callbacks, count accumulation, disarm and continuous clock. */
 long timer=SC(85,1,0x80800,0,0);CHECK(timer>=0&&ctl(ep,1,timer,1,88)==0);
 struct timer it={.value={0,30000000}},old={0},snapshot={0};
 CHECK(SC(86,timer,0,&it,&old)==0&&old.value.sec==0&&old.value.nsec==0);
 CHECK(SC(87,timer,&snapshot,0,0)==0&&snapshot.value.sec==0&&snapshot.value.nsec<=30000000);
 CHECK(wait(ep,events,4,1000)==1&&events[0].data==88&&events[0].events==1);
 CHECK(SC(63,timer,&value,8,0)==8&&value==1&&SC(63,timer,&value,8,0)==-11);
 CHECK(SC(86,timer,0,1,0)==-14);it=(struct timer){.interval={0,10000000},.value={0,10000000}};
 CHECK(SC(86,timer,0,&it,0)==0 && delay(100000000)==0);
 CHECK(SC(63,timer,&value,8,0)==8&&value>=3);
 it=(struct timer){0};CHECK(SC(86,timer,0,&it,0)==0&&SC(63,timer,&value,8,0)==-11);
 CHECK(ctl(ep,2,timer,0,0)==0&&closefd(timer)==0);
 timer=SC(85,7,0x80800,0,0);CHECK(timer>=0);struct time now;CHECK(SC(113,7,&now,0,0)==0);
 it=(struct timer){.value=now};it.value.nsec+=20000000;if(it.value.nsec>=1000000000){it.value.sec++;it.value.nsec-=1000000000;}
 CHECK(ctl(ep,1,timer,1,99)==0 && SC(86,timer,1,&it,0)==0);
 CHECK(wait(ep,events,4,1000)==1&&events[0].data==99&&SC(63,timer,&value,8,0)==8&&value==1);
 CHECK(ctl(ep,2,timer,0,0)==0&&closefd(timer)==0);
 for(int i=0;i<32;i++){timer=SC(85,1,0x80800,0,0);CHECK(timer>=0);it=(struct timer){.value={0,1000}};CHECK(SC(86,timer,0,&it,0)==0&&closefd(timer)==0);}
 SAY("LINUX_TIMERFD_PASS\n");
 /* Native blocked process signals, coalescing/mask update and real SIGCHLD. */
 u64 mask=(1UL<<9)|(1UL<<11)|(1UL<<16),oldmask=0;CHECK(SC(135,0,&mask,&oldmask,8)==0);
 u64 selected=1UL<<9;long sigfd=SC(74,-1,&selected,8,0x80800);CHECK(sigfd>=0&&ctl(ep,1,sigfd,1,111)==0);
 struct signal_record record;long parent=SC(172,0,0,0,0),uid=SC(174,0,0,0,0);
 CHECK(SC(63,sigfd,&record,128,0)==-11 && SC(129,parent,10,0,0)==0&&SC(129,parent,10,0,0)==0);
 CHECK(wait(ep,events,4,0)==1&&events[0].data==111&&SC(63,sigfd,&record,127,0)==-22);
 CHECK(SC(63,sigfd,&record,128,0)==128&&record.signo==10&&record.pid==(unsigned)parent&&record.uid==(unsigned)uid&&record.code==0);
 CHECK(SC(63,sigfd,&record,128,0)==-11&&SC(129,parent,12,0,0)==0&&wait(ep,events,4,0)==0);
 selected=(1UL<<9)|(1UL<<11)|(1UL<<16);CHECK(SC(74,sigfd,&selected,8,0)==sigfd&&SC(25,sigfd,3,0,0)==0x802);
 CHECK(wait(ep,events,4,0)==1&&SC(63,sigfd,1,128,0)==-14&&SC(63,sigfd,&record,128,0)==-11);
 child=SC(220,17,0,0,0);CHECK(child>=0);if(!child)return 37;
 CHECK(wait(ep,events,4,1000)==1&&SC(63,sigfd,&record,128,0)==128);
 CHECK(record.signo==17&&record.pid==(unsigned)child&&record.uid==(unsigned)uid&&record.code==1&&record.status==37&&reap(child,37)==0);
 u64 usr1=1UL<<9;long blocking=SC(74,-1,&usr1,8,0x80000);CHECK(blocking>=0);
 child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){SAY("REACTOR_SIGNAL_CHILD_READY\n");char token=0;CHECK(SC(63,3,&token,1,0)==1&&token=='T');CHECK(SC(129,parent,10,0,0)==0);return 0;}
 SAY("REACTOR_SIGNAL_WAIT\n");CHECK(SC(63,blocking,&record,128,0)==128&&record.signo==10&&record.pid==(unsigned)child);
 CHECK(reap(child,0)==0&&SC(63,sigfd,&record,128,0)==128&&record.signo==17);
 CHECK(ctl(ep,2,sigfd,0,0)==0&&closefd(sigfd)==0&&closefd(blocking)==0);
 CHECK(SC(135,2,&oldmask,0,8)==0);SAY("LINUX_SIGNALFD_PASS\n");
 CHECK(closefd(ep)==0);SAY("LINUX_REACTOR_PASS\n");return 0;
}
__asm__(".text\n.global _start\n_start:\nmov x0,sp\nbl probe_main\nmov x8,#94\nsvc #0\nb .\n");
