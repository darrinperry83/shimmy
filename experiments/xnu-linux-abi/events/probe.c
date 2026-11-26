/* Same Linux ELF on both kernels: eventfd counter/lifetime and real wakeups. */
typedef unsigned long u64;
static long call(u64 n,u64 a,u64 b,u64 c,u64 d,u64 e){
 register u64 x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=0,x8 __asm__("x8")=n;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");return (long)x0;
}
#define SC(n,a,b,c,d) call(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d),0)
#define CHECK(e) do{if(!(e)){static const char msg[]="EVENTS_CHECK " #e "\n";SC(64,2,msg,sizeof(msg)-1,0);return 111;}}while(0)
#define SAY(s) do{static const char msg[]=s;CHECK(SC(64,1,msg,sizeof(msg)-1,0)==sizeof(msg)-1);}while(0)
struct pollfd{int fd;short events,revents;};
struct time{long sec,nsec;};
struct info{int signo,error,code,pad,pid;unsigned uid;int status,pad2;long times[2];u64 spare[10];};
static int eq(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static long number(const char *s){long n=0;while(*s)n=n*10+*s++-'0';return n;}
static void digits(char *s,long n){char tmp[24];int k=0;do{tmp[k++]=(char)('0'+n%10);n/=10;}while(n);for(int i=0;i<k;i++)s[i]=tmp[k-1-i];s[k]=0;}
static long writeval(long fd,u64 n){return SC(64,fd,&n,8,0);}
static long pollnow(long fd,short bits,short *out){struct pollfd f={(int)fd,bits,0};struct time t={0,0};long n=SC(73,&f,1,&t,0);*out=f.revents;return n;}
static long reap(long pid){struct info e={0};long n=SC(95,1,pid,&e,4);return n?n:(e.pid==pid&&e.code==1&&e.status==0?0:-1);}
long probe_main(u64 *stack){
 u64 value=0;short bits=0;
 if(stack[0]==4&&eq((const char *)stack[2],"exec-child")){
  long held=number((const char *)stack[3]),clo=number((const char *)stack[4]);
  CHECK(SC(63,clo,&value,8,0)==-9);
  CHECK(SC(63,held,&value,8,0)==8&&value==7);SAY("EVENTS_EXEC_FD_PASS\n");return 0;
 }
 CHECK(stack[0]==1);const char *self=(const char *)stack[1];
 CHECK(SC(19,0,2,0,0)==-22);
 long fd=SC(19,0,0x80800,0,0);CHECK(fd>=0);
 CHECK(SC(25,fd,1,0,0)==1 && SC(25,fd,3,0,0)==0x802);
 CHECK(SC(63,fd,&value,8,0)==-11);
 CHECK(SC(63,fd,&value,0,0)==-22 && SC(64,fd,&value,0,0)==-22);
 CHECK(SC(63,fd,&value,7,0)==-22 && SC(64,fd,&value,9,0)==8);
 CHECK(pollnow(fd,5,&bits)==1&&bits==4);
 CHECK(writeval(fd,3)==8&&writeval(fd,4)==8&&pollnow(fd,5,&bits)==1&&bits==5);
 u64 pair[2]={0,0x4321};CHECK(SC(63,fd,pair,16,0)==8 && pair[0]==7&&pair[1]==0x4321);
 CHECK(writeval(fd,~0UL)==-22 && SC(64,fd,1,8,0)==-14);
 CHECK(writeval(fd,9)==8 && SC(63,fd,1,8,0)==-14 && SC(63,fd,&value,8,0)==-11);
 CHECK(writeval(fd,~0UL-1)==8 && pollnow(fd,5,&bits)==1&&bits==1);
 CHECK(writeval(fd,1)==-11 && writeval(fd,0)==8);
 CHECK(SC(63,fd,&value,8,0)==8&&value==~0UL-1);
 long sem=SC(19,2,0x801,0,0);CHECK(sem>=0);
 CHECK(SC(63,sem,&value,8,0)==8&&value==1 && SC(63,sem,&value,8,0)==8&&value==1);
 CHECK(SC(63,sem,&value,8,0)==-11);CHECK(SC(57,sem,0,0,0)==0);
 long alias=SC(25,fd,0,20,0);CHECK(alias>=20);
 CHECK(writeval(fd,5)==8&&SC(57,fd,0,0,0)==0);
 CHECK(SC(63,alias,&value,8,0)==8&&value==5);
 CHECK(pollnow((long)fd,1,&bits)==1&&bits==32);
 struct time invalid={0,1000000000};CHECK(SC(73,0,0,&invalid,0)==-22);
 CHECK(SC(73,1,1,0,0)==-14);
 struct time start,end,t={0,10000000};CHECK(SC(113,1,&start,0,0)==0);
 CHECK(SC(73,0,0,&t,0)==0&&t.sec==0&&t.nsec==0);
 CHECK(SC(113,1,&end,0,0)==0 && (end.sec-start.sec)*1000000000+end.nsec-start.nsec>=10000000);
 for(int phase=1;phase<=3;phase++){
  long block=SC(19,0,0,0,0);CHECK(block>=0);
  if(phase==3)CHECK(writeval(block,~0UL-1)==8);
  long pid=SC(220,17,0,0,0);CHECK(pid>=0);
  if(!pid){
   if(phase==1)SAY("EVENT_CHILD_1\n");if(phase==2)SAY("EVENT_CHILD_2\n");if(phase==3)SAY("EVENT_CHILD_3\n");
   char c=0;CHECK(SC(63,3,&c,1,0)==1&&c=='T');
   if(phase==3){CHECK(SC(63,block,&value,8,0)==8&&value==~0UL-1);}
   else CHECK(writeval(block,9)==8);
   return 0;
  }
  if(phase==1){SAY("EVENT_PHASE_1\n");struct pollfd f={(int)block,1,0};CHECK(SC(73,&f,1,0,0)==1&&f.revents==1);}
  if(phase==2)SAY("EVENT_PHASE_2\n");
  if(phase==3){SAY("EVENT_PHASE_3\n");CHECK(writeval(block,1)==8);}
  CHECK(SC(63,block,&value,8,0)==8&&value==(phase==3?1UL:9UL));
  CHECK(reap(pid)==0&&SC(57,block,0,0,0)==0);
 }
 long held=SC(19,7,0x800,0,0),clo=SC(19,8,0x80000,0,0);CHECK(held>=0&&clo>=0);
 char a[24],b[24];digits(a,held);digits(b,clo);
 long pid=SC(220,17,0,0,0);CHECK(pid>=0);
 if(!pid){const char *args[]={self,"exec-child",a,b,0},*env[]={0};SC(221,self,args,env,0);return 112;}
 CHECK(reap(pid)==0 && SC(25,clo,1,0,0)==1);
 CHECK(SC(63,held,&value,8,0)==-11); /* Shared fileglob was consumed by exec child. */
 CHECK(SC(57,held,0,0,0)==0&&SC(57,clo,0,0,0)==0&&SC(57,alias,0,0,0)==0);
 SAY("LINUX_EVENTS_PASS\n");return 0;
}
__asm__(".text\n.global _start\n_start:\nmov x0,sp\nbl probe_main\nmov x8,#94\nsvc #0\nb .\n");
