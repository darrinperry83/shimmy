/* Same freestanding ELF on Linux and XNU: init must own real child lifecycle.
 * Reuses process/probe.c's raw ABI and reactor's blocked-signal contract.
 * No libc, procfs boot marker, PID override, or userspace child registry. */
typedef unsigned long u64;
static long sc(u64 n,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f){
 register u64 x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
 register u64 x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f,x8 __asm__("x8")=n;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");
 return (long)x0;
}
#define SC(n,a,b,c,d) sc(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d),0,0)
#define CHECK(e) do{if(!(e)){static const char m[]="INIT_CHECK " #e "\n";SC(64,2,m,sizeof(m)-1,0);return stage;}}while(0)
struct info {int signo,error,code,pad,pid;unsigned uid;int status,pad2;long utime,stime;u64 spare[10];};
struct signal_info {unsigned signo;int error,code;unsigned pid,uid;int fd;unsigned tid,band,overrun,trapno;int status,value;u64 pointer,utime,stime,addr;unsigned short addr_lsb,pad;int syscall;u64 call_addr;unsigned arch;unsigned char spare[28];};
_Static_assert(sizeof(struct info)==128,"waitid ABI");
_Static_assert(sizeof(struct signal_info)==128,"signalfd ABI");
static long waitid(int pid,struct info *out,int flags){return sc(95,pid?1:0,pid,(u64)out,flags,0,0);}
static int closefd(int fd){return (int)SC(57,fd,0,0,0);}
static int readnum(int fd,long *value){return SC(63,fd,value,sizeof(*value),0)==sizeof(*value);}
static int writenum(int fd,long value){return SC(64,fd,&value,sizeof(value),0)==sizeof(value);}
static void exitnow(int status){SC(94,status,0,0,0);for(;;)__asm__ volatile("yield");}
static int eq(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static char *number(char *end,long value){*--end=0;do{*--end='0'+value%10;value/=10;}while(value);return end;}
static long parse(const char *p){long n=0;while(*p>='0'&&*p<='9')n=n*10+*p++-'0';return n;}
static volatile unsigned clear_tid;

static int subreaper_owner(void){
 int stage=165,enabled=0,report[2],release[2];struct info event;
 long adopter=SC(172,0,0,0,0);
 /* The flag was set before exec, and must survive the native proc switch. */
 CHECK(SC(167,37,&enabled,0,0)==0&&enabled==1);
 CHECK(SC(167,37,1,0,0)==-14);
 CHECK(SC(59,report,0,0,0)==0&&SC(59,release,0,0,0)==0);
 long middle=SC(220,17,0,0,0);CHECK(middle>=0);
 if(!middle){
  enabled=-1;CHECK(SC(167,37,&enabled,0,0)==0&&enabled==0);
  CHECK(closefd(report[0])==0&&closefd(release[1])==0);
  long leaf=SC(220,17,0,0,0);CHECK(leaf>=0);
  if(!leaf){long token;CHECK(writenum(report[1],SC(172,0,0,0,0)));
   CHECK(readnum(release[0],&token)&&token==72&&SC(173,0,0,0,0)==adopter);return 46;}
  return 45;
 }
 long leaf;CHECK(closefd(report[1])==0&&closefd(release[0])==0&&readnum(report[0],&leaf));
 CHECK(waitid(middle,&event,4)==0&&event.pid==middle&&event.status==45);
 CHECK(writenum(release[1],72)&&waitid(leaf,&event,4)==0&&event.pid==leaf&&event.status==46);
 CHECK(waitid(0,&event,5)==-10&&closefd(report[0])==0&&closefd(release[1])==0);
 /* Also adopt an already-waitable zombie, without the old parent's reap. */
 CHECK(SC(59,report,0,0,0)==0);middle=SC(220,17,0,0,0);CHECK(middle>=0);
 if(!middle){
  CHECK(closefd(report[0])==0);leaf=SC(220,17,0,0,0);CHECK(leaf>=0);
  if(!leaf)return 48;
  CHECK(waitid(leaf,&event,4|0x1000000)==0&&event.pid==leaf&&event.status==48);
  CHECK(writenum(report[1],leaf));return 49;
 }
 CHECK(closefd(report[1])==0&&readnum(report[0],&leaf));
 CHECK(waitid(middle,&event,4)==0&&event.pid==middle&&event.status==49);
 CHECK(waitid(leaf,&event,4)==0&&event.pid==leaf&&event.status==48);
 CHECK(waitid(leaf,&event,5)==-10&&closefd(report[0])==0);
 CHECK(SC(167,36,0,0,0)==0&&SC(167,37,&enabled,0,0)==0&&enabled==0);
 return 47;
}

long probe_main(u64 *initial){
 int stage=160;long argc=initial[0];char **argv=(char **)(initial+1);
 if(argc==2&&eq(argv[1],"subreaper-owner"))return subreaper_owner();
 int subreaper_only=argc==2&&eq(argv[1],"subreaper-only");
 struct info event;long child;int report[2],release[2];
 if(subreaper_only){CHECK(SC(172,0,0,0,0)>1);goto subreaper_gate;}
 if(argc==4&&eq(argv[1],"exec-child")){
  stage=161;long pid=parse(argv[2]),parent=parse(argv[3]);
  CHECK(SC(172,0,0,0,0)==pid&&SC(173,0,0,0,0)==parent);
  CHECK(SC(178,0,0,0,0)==pid&&SC(96,&clear_tid,0,0,0)==pid);
  return 37;
 }
 CHECK(argc==1|| (argc==2&&eq(argv[1],"exit-with-child")));
 CHECK(SC(172,0,0,0,0)==1&&SC(173,0,0,0,0)==0);
 CHECK(SC(178,0,0,0,0)==1&&SC(96,&clear_tid,0,0,0)==1);
 /* Init's session and process group must agree across all query surfaces. */
 long sid=SC(156,0,0,0,0);
 if(sid!=1)CHECK(SC(157,0,0,0,0)==1);
 CHECK(SC(156,0,0,0,0)==1&&SC(155,0,0,0,0)==1);
 CHECK(waitid(0,&event,5)==-10);
 u64 blocked=(1UL<<16)|(1UL<<9); /* SIGCHLD and SIGUSR1; native pending state. */
 CHECK(SC(135,0,&blocked,0,8)==0);
 long signal_fd=SC(74,-1,&blocked,8,0x80800);CHECK(signal_fd>=0);

 stage=162;child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){
  char pidbuf[24],parentbuf[24];
  char *args[]={argv[0],"exec-child",number(pidbuf+sizeof(pidbuf),SC(172,0,0,0,0)),number(parentbuf+sizeof(parentbuf),SC(173,0,0,0,0)),0};
  char *env[]={0};SC(221,argv[0],args,env,0);return 162;
 }
 CHECK(child>1&&waitid(child,&event,4|0x1000000)==0);
 CHECK(event.pid==child&&event.signo==17&&event.code==1&&event.status==37);
 CHECK(waitid(child,&event,4)==0&&event.pid==child&&event.status==37);
 CHECK(waitid(child,&event,5)==-10);
 struct signal_info record;
 CHECK(SC(63,signal_fd,&record,sizeof(record),0)==sizeof(record));
 CHECK(record.signo==17&&record.pid==(unsigned)child&&record.code==1&&record.status==37);

 /* A living grandchild cannot observe adoption until its parent is reaped. */
 stage=163;CHECK(SC(59,report,0,0,0)==0&&SC(59,release,0,0,0)==0);
 child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){
  CHECK(closefd(report[0])==0&&closefd(release[1])==0);
  long grandchild=SC(220,17,0,0,0);CHECK(grandchild>=0);
  if(!grandchild){
   CHECK(writenum(report[1],SC(172,0,0,0,0)));long token;
   CHECK(readnum(release[0],&token)&&token==71);
   CHECK(SC(173,0,0,0,0)==1&&SC(178,0,0,0,0)==SC(172,0,0,0,0));
   return 41;
  }
  return 40;
 }
 CHECK(closefd(report[1])==0&&closefd(release[0])==0);long orphan;
 CHECK(readnum(report[0],&orphan)&&orphan>1&&orphan!=child);
 CHECK(waitid(child,&event,4)==0&&event.pid==child&&event.status==40);
 CHECK(writenum(release[1],71));
 int status=-1;
 CHECK(SC(260,orphan,&status,0,0)==orphan&&status==(41<<8));
 CHECK(waitid(orphan,&event,5)==-10&&closefd(report[0])==0&&closefd(release[1])==0);

 /* Already-dead grandchildren must remain waitable by their new init. */
 stage=164;CHECK(SC(59,report,0,0,0)==0);
 child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){
  CHECK(closefd(report[0])==0);long grandchild=SC(220,17,0,0,0);CHECK(grandchild>=0);
  if(!grandchild)return 43;
  CHECK(waitid(grandchild,&event,4|0x1000000)==0&&event.pid==grandchild&&event.status==43);
  CHECK(writenum(report[1],grandchild));return 42;
 }
 CHECK(closefd(report[1])==0&&readnum(report[0],&orphan));
 CHECK(waitid(child,&event,4)==0&&event.pid==child&&event.status==42);
 CHECK(waitid(orphan,&event,4)==0&&event.pid==orphan&&event.code==1&&event.status==43);
 CHECK(waitid(0,&event,5)==-10&&closefd(report[0])==0);

 /* A subreaper adopts descendants before domain init does. */
subreaper_gate:
 stage=165;child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){
  int enabled=-1;CHECK(SC(167,37,&enabled,0,0)==0&&enabled==0);
  CHECK(SC(167,36,1UL<<32,0,0)==0&&SC(167,37,&enabled,0,0)==0&&enabled==1);
  char *args[]={argv[0],"subreaper-owner",0},*env[]={0};
  SC(221,argv[0],args,env,0);return 165;
 }
 CHECK(waitid(child,&event,4)==0&&event.pid==child&&event.status==47);
 CHECK(waitid(0,&event,5)==-10);
 if(subreaper_only){
  static const char pass[]="LINUX_SUBREAPER_PASS\n";
  CHECK(SC(64,1,pass,sizeof(pass)-1,0)==sizeof(pass)-1);return 0;
 }

 /* Real blocked user signal must report the child's domain identity. */
 stage=166;child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){CHECK(SC(129,1,10,0,0)==0);return 44;}
 CHECK(waitid(child,&event,4)==0&&event.pid==child&&event.status==44);
 int got_user=0;
 for(unsigned i=0;i<2;i++){
  long result=SC(63,signal_fd,&record,sizeof(record),0);
  if(result==-11)break;
  CHECK(result==sizeof(record));
  if(record.signo==10){CHECK(record.pid==(unsigned)child);got_user=1;}
  else CHECK(record.signo==17);
 }
 CHECK(got_user&&closefd(signal_fd)==0);
 if(argc==2){
  stage=167;CHECK(SC(59,report,0,0,0)==0);child=SC(220,17,0,0,0);CHECK(child>=0);
  if(!child){closefd(report[0]);CHECK(writenum(report[1],SC(172,0,0,0,0)));for(;;)__asm__ volatile("yield");}
  CHECK(closefd(report[1])==0&&readnum(report[0],&orphan));
  static const char dying[]="LINUX_INIT_EXIT_WITH_LIVE_CHILD\n";SC(64,1,dying,sizeof(dying)-1,0);
  exitnow(0);
 }
 static const char pass[]="LINUX_INIT_LIFECYCLE_PASS\n";
 CHECK(SC(64,1,pass,sizeof(pass)-1,0)==sizeof(pass)-1);return 0;
}
__asm__(".text\n.global _start\n_start:\nmov x0,sp\nbl probe_main\nmov x8,#94\nsvc #0\nb .\n");
