/* Unchanged freestanding Linux ELF: owned-domain lifecycle on native XNU IDs.
 * PID 1 mapping is intentionally not asserted by this gate. */
typedef unsigned long u64;
static long sc(u64 n,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f){
 register u64 x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
 register u64 x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f,x8 __asm__("x8")=n;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");return (long)x0;
}
#define SC(n,a,b,c,d) sc(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d),0,0)
#define CHECK(e) do{if(!(e)){static const char message[]="DOMAIN_CHECK " #e "\n";SC(64,2,message,sizeof(message)-1,0);return stage;}}while(0)
struct info{int signo,error,code,pad,pid;unsigned uid;int status,pad2;long utime,stime;u64 spare[10];};
static long parse(const char *p){long n=0;while(*p>='0'&&*p<='9')n=n*10+*p++-'0';return n;}
static char *number(char *end,long value){*--end=0;do{*--end='0'+value%10;value/=10;}while(value);return end;}
static int eq(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static long waitid(long pid,struct info *info,int flags){return sc(95,pid?1:0,pid,(u64)info,flags,0,0);}
static int readnum(int fd,long *n){return SC(63,fd,n,sizeof(*n),0)==sizeof(*n);}
static int writenum(int fd,long n){return SC(64,fd,&n,sizeof(n),0)==sizeof(n);}
static volatile unsigned clear_tid;
static void wait_diagnostic(long result,long expected,struct info *event){
 static const char label[]="DOMAIN_WAIT result expected pid status code: ";SC(64,2,label,sizeof(label)-1,0);
 long values[]={result,expected,event->pid,event->status,event->code};
 for(unsigned i=0;i<5;i++){char buffer[32];long n=values[i];if(n<0){SC(64,2,"-",1,0);n=-n;}
  char *text=number(buffer+sizeof(buffer),n);unsigned length=0;while(text[length])length++;
  SC(64,2,text,length,0);SC(64,2,i==4?"\n":" ",1,0);}
}

long probe_main(u64 *initial){
 int stage=170;long argc=initial[0];char **argv=(char **)(initial+1);
 if(argc==5&&eq(argv[1],"exec-member")){
  CHECK(SC(172,0,0,0,0)==parse(argv[2])&&SC(173,0,0,0,0)==parse(argv[3]));
  CHECK(SC(129,parse(argv[4]),0,0,0)==-3);
  return 37;
 }
 CHECK(argc==5&&eq(argv[1],"member"));long outsider=parse(argv[2]);
 long root=SC(172,0,0,0,0);CHECK(root>0&&SC(173,0,0,0,0)==0);
 CHECK(SC(178,0,0,0,0)==root&&SC(96,&clear_tid,0,0,0)==root);
 /* The native test supplies a real same-UID process in another domain. The
  * Linux reference supplies a nonexistent PID; only the native gate proves
  * rejection of that live cross-domain target. */
 CHECK(SC(129,outsider,0,0,0)==-3&&SC(129,outsider,10,0,0)==-3);
 CHECK(SC(155,outsider,0,0,0)==-3&&SC(156,outsider,0,0,0)==-3);
 CHECK(SC(154,outsider,0,0,0)==-3&&SC(261,outsider,7,0,0)==-3);
 struct info event;CHECK(waitid(0,&event,5)==-10);
 long child=SC(220,17,0,0,0);CHECK(child>0||child==0);
 if(!child){
  char pidbuf[24],parentbuf[24];
  char *args[]={argv[0],"exec-member",number(pidbuf+sizeof(pidbuf),SC(172,0,0,0,0)),number(parentbuf+sizeof(parentbuf),SC(173,0,0,0,0)),argv[2],0};
  char *env[]={0};SC(221,argv[0],args,env,0);return stage;
 }
 CHECK(waitid(child,&event,4)==0&&event.pid==child&&event.status==37);
 CHECK(waitid(child,&event,5)==-10);

 /* The domain leader adopts descendants even without the subreaper flag. */
 stage=171;int enabled=-1,report[2],release[2];
 CHECK(SC(167,37,&enabled,0,0)==0&&enabled==0);
 CHECK(SC(59,report,0,0,0)==0&&SC(59,release,0,0,0)==0);
 child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){
  SC(57,report[0],0,0,0);SC(57,release[1],0,0,0);
  long leaf=SC(220,17,0,0,0);CHECK(leaf>=0);
  if(!leaf){long token;CHECK(writenum(report[1],SC(172,0,0,0,0)));
   CHECK(readnum(release[0],&token)&&token==51&&SC(173,0,0,0,0)==root);return 41;}
  return 40;
 }
 SC(57,report[1],0,0,0);SC(57,release[0],0,0,0);long leaf;
 CHECK(readnum(report[0],&leaf)&&waitid(child,&event,4)==0&&event.status==40);
 CHECK(writenum(release[1],51));int status=0;
 CHECK(SC(260,leaf,&status,0,0)==leaf&&status==(41<<8));
 CHECK(waitid(leaf,&event,5)==-10);
 SC(57,report[0],0,0,0);SC(57,release[1],0,0,0);

 stage=172;CHECK(SC(59,report,0,0,0)==0);child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){
  SC(57,report[0],0,0,0);leaf=SC(220,17,0,0,0);CHECK(leaf>=0);
  if(!leaf)return 43;
  CHECK(waitid(leaf,&event,4|0x1000000)==0&&event.pid==leaf&&event.status==43);
  CHECK(writenum(report[1],leaf));return 42;
 }
 SC(57,report[1],0,0,0);CHECK(readnum(report[0],&leaf));
 CHECK(waitid(child,&event,4)==0&&event.pid==child&&event.status==42);
 long adopted=waitid(leaf,&event,4);
 if(adopted||event.pid!=leaf||event.status!=43)wait_diagnostic(adopted,leaf,&event);
 CHECK(adopted==0&&event.pid==leaf&&event.status==43);
 CHECK(waitid(0,&event,5)==-10);SC(57,report[0],0,0,0);

 stage=173;child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){for(;;)__asm__ volatile("yield");}
 CHECK(SC(129,child,0,0,0)==0&&SC(129,child,15,0,0)==0);
 CHECK(waitid(child,&event,4)==0&&event.pid==child&&event.code==2&&event.status==15);
 CHECK(SC(129,child,0,0,0)==-3);
 /* Reuse the qualified subreaper ELF inside this owned domain. */
 child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){char *args[]={argv[4],"subreaper-only",0},*env[]={0};SC(221,args[0],args,env,0);return stage;}
 CHECK(waitid(child,&event,4)==0&&event.pid==child&&event.status==0);
 if(eq(argv[3],"native")){
  char *args[]={"/bin/echo","should-not-exec",0},*env[]={0};
  CHECK(SC(221,args[0],args,env,0)==-8); /* Reject Darwin exec out of this domain. */
 }
 /* Kernel closure must kill this live descendant and release its domain
  * reference. The external native supervisor checks EOF and the live count. */
 stage=174;CHECK(SC(59,report,0,0,0)==0);child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){SC(57,report[0],0,0,0);CHECK(writenum(report[1],SC(172,0,0,0,0)));
  for(;;)__asm__ volatile("yield");}
 SC(57,report[1],0,0,0);CHECK(readnum(report[0],&leaf)&&leaf==child);
 static const char pass[]="LINUX_DOMAIN_LIFECYCLE_PASS\n";
 CHECK(SC(64,1,pass,sizeof(pass)-1,0)==sizeof(pass)-1);return 0;
}
__asm__(".text\n.global _start\n_start:\nmov x0,sp\nbl probe_main\nmov x8,#94\nsvc #0\nb .\n");
