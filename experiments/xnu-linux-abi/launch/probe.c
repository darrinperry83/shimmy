/* Unchanged Linux AArch64 ELF: native service-launch controls. */
typedef unsigned long u64;
static long call(u64 n,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f){
 register u64 x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f,x8 __asm__("x8")=n;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");return (long)x0;
}
#define SC(n,a,b,c,d) call(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d),0,0)
#define CHECK(e) do{if(!(e)){static const char msg[]="LAUNCH_CHECK " #e "\n";SC(64,2,msg,sizeof(msg)-1,0);return 113;}}while(0)
#define SAY(s) do{static const char msg[]=s;CHECK(SC(64,1,msg,sizeof(msg)-1,0)==sizeof(msg)-1);}while(0)
struct time {long sec,usec;};
struct usage {struct time user,system;long values[14];};
struct limits {u64 soft,hard;};
static volatile unsigned char resident[2*1024*1024];
struct stat {u64 dev,ino;unsigned mode,nlink,uid,gid;u64 rdev,pad;long size;int blksize,pad2;long blocks;u64 rest[8];};
static int eq(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static long closefd(long fd){return SC(57,fd,0,0,0);}
static long reap(long pid,int expected){int status=0;long n=SC(260,pid,&status,0,0);return n==pid&&status==expected?0:-1;}
long probe_main(u64 *stack){
 if(stack[0]==2){
  CHECK(eq((const char *)stack[2],"exec-check"));CHECK(SC(25,45,1,0,0)==-9&&SC(25,1,1,0,0)==0);
  SAY("ARCH_LAUNCH_EXEC_READY\n");const char *args[]={"/usr/bin/printf","ARCH_LAUNCH_CHILD_PASS\n",0},*env[]={0};
  SC(221,args[0],args,env,0);return 114;
 }
 CHECK(stack[0]==1);int pair[2];
 CHECK(SC(59,pair,1,0,0)==-22&&SC(59,1,0x80800,0,0)==-14);
 CHECK(SC(59,pair,0x80800,0,0)==0);int readfd=pair[0],writefd=pair[1];char byte=0;
 CHECK(SC(25,readfd,1,0,0)==1&&SC(25,writefd,1,0,0)==1);
 CHECK((SC(25,readfd,3,0,0)&0x800)&&SC(63,readfd,&byte,1,0)==-11);
 CHECK(SC(64,writefd,"ab",2,0)==2);long alias=SC(23,readfd,0,0,0);CHECK(alias>=0&&SC(25,alias,1,0,0)==0);
 CHECK(SC(24,readfd,40,0x80000,0)==40&&SC(25,40,1,0,0)==1);
 CHECK(SC(24,readfd,readfd,0,0)==-22&&SC(24,-1,41,0,0)==-9&&SC(24,readfd,41,1,0)==-22);
 long high=SC(25,readfd,1030,50,0);CHECK(high>=50&&SC(25,high,1,0,0)==1);
 CHECK(SC(63,alias,&byte,1,0)==1&&byte=='a'&&SC(63,40,&byte,1,0)==1&&byte=='b');
 CHECK(SC(25,alias,4,0,0)==0&&!(SC(25,readfd,3,0,0)&0x800));
 CHECK(closefd(writefd)==0&&SC(63,readfd,&byte,1,0)==0);
 CHECK(closefd(alias)==0&&closefd(high)==0&&closefd(40)==0&&closefd(readfd)==0);
 SAY("LINUX_LAUNCH_DESCRIPTORS_PASS\n");
 long oldmask=SC(166,027,0,0,0);CHECK(oldmask>=0);
 long file=SC(56,-100,"mask-file",0xc2,0666);CHECK(file>=0);struct stat st={0};
 CHECK(SC(80,file,&st,0,0)==0&&(st.mode&0777)==0640&&closefd(file)==0);
 CHECK(SC(166,077,0,0,0)==027&&SC(166,027,0,0,0)==077);
 struct limits original={0},before={0},now={0};CHECK(SC(261,0,7,0,&original)==0);
 CHECK(SC(163,7,&now,0,0)==0&&now.soft==original.soft&&now.hard==original.hard);
 CHECK(SC(163,7,0,0,0)==-14&&SC(164,7,0,0,0)==-14);
 CHECK(SC(261,0,999,0,&now)==-22&&SC(261,0,7,0,1)==-14);
 long child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){
  CHECK(SC(166,077,0,0,0)==027);struct limits reduced={32,32};
  CHECK(SC(261,0,7,&reduced,&before)==0&&before.soft==original.soft&&before.hard==original.hard);
  CHECK(SC(163,7,&now,0,0)==0&&now.soft==32&&now.hard==32);
  CHECK(SC(25,0,0,32,0)==-22&&SC(24,0,32,0,0)==-9);
  reduced=(struct limits){33,33};CHECK(SC(164,7,&reduced,0,0)==-1);
  reduced=(struct limits){31,32};CHECK(SC(261,0,7,&reduced,1)==-14);
  CHECK(SC(163,7,&now,0,0)==0&&now.soft==31&&now.hard==32);
  reduced=(struct limits){33,32};CHECK(SC(164,7,&reduced,0,0)==-22);
  return 37;
 }
 CHECK(reap(child,37<<8)==0&&SC(166,oldmask,0,0,0)==027);
 CHECK(SC(163,7,&now,0,0)==0&&now.soft==original.soft&&now.hard==original.hard);
 SAY("LINUX_LAUNCH_LIMITS_PASS\n");
 CHECK(SC(155,0,0,0,0)>0&&SC(156,0,0,0,0)>0);
 CHECK(SC(59,pair,0x80000,0,0)==0);child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){CHECK(closefd(pair[0])==0&&SC(154,0,0,0,0)==0);
  for(unsigned i=0;i<sizeof(resident);i+=4096)resident[i]=1;
  CHECK(SC(155,0,0,0,0)==SC(172,0,0,0,0)&&SC(64,pair[1],"G",1,0)==1);
  CHECK(SC(63,3,&byte,1,0)==1&&byte=='T');return 0;}
 CHECK(closefd(pair[1])==0&&SC(63,pair[0],&byte,1,0)==1&&byte=='G');
 CHECK(SC(155,child,0,0,0)==child);int status=0x12345678;struct usage usage={0};
 CHECK(SC(260,child,&status,1,&usage)==0&&status==0x12345678);
 SAY("LAUNCH_GROUP_CHILD_READY\n");SAY("LAUNCH_GROUP_WAIT\n");
 CHECK(SC(260,child,&status,0,&usage)==child&&status==0&&closefd(pair[0])==0);
 CHECK(usage.user.sec>=0&&usage.user.usec>=0&&usage.user.usec<1000000&&usage.values[0]>0&&usage.values[0]<1000000000);
 CHECK(SC(59,pair,0,0,0)==0);child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){CHECK(closefd(pair[1])==0);SC(63,pair[0],&byte,1,0);return 115;}
 CHECK(closefd(pair[0])==0&&SC(129,child,10,0,0)==0&&reap(child,10)==0&&closefd(pair[1])==0);
 SAY("LINUX_LAUNCH_WAIT_PASS\n");
 CHECK(SC(59,pair,0x80000,0,0)==0);child=SC(220,17,0,0,0);CHECK(child>=0);
 if(!child){
  long pid=SC(172,0,0,0,0);CHECK(SC(157,0,0,0,0)==pid&&SC(156,0,0,0,0)==pid&&SC(155,0,0,0,0)==pid);
  CHECK(SC(157,0,0,0,0)==-1&&SC(154,0,0,0,0)==-1);
  CHECK(closefd(pair[0])==0&&SC(24,pair[1],1,0,0)==1&&SC(24,pair[1],45,0x80000,0)==45&&closefd(pair[1])==0);
  CHECK(SC(64,1,"R",1,0)==1&&SC(63,3,&byte,1,0)==1&&byte=='T');
  const char *args[]={"/usr/bin/launch-probe","exec-check",0},*env[]={0};SC(221,args[0],args,env,0);return 116;
 }
 CHECK(closefd(pair[1])==0&&SC(63,pair[0],&byte,1,0)==1&&byte=='R');
 SAY("LAUNCH_EXEC_CHILD_READY\n");SAY("LAUNCH_EXEC_WAIT\n");
 CHECK(SC(260,child,&status,0,&usage)==child&&status==0);
 char output[128]={0};long used=0,n;
 while((n=SC(63,pair[0],output+used,sizeof(output)-1-used,0))>0){used+=n;CHECK((u64)used<sizeof(output)-1);}
 CHECK(n==0&&eq(output,"ARCH_LAUNCH_EXEC_READY\nARCH_LAUNCH_CHILD_PASS\n")&&closefd(pair[0])==0);
 SAY("ARCH_LAUNCH_CHILD_PASS\n");CHECK(SC(260,-1,&status,1,0)==-10);
 SAY("LINUX_LAUNCH_PASS\n");return 0;
}
__attribute__((naked,noreturn)) void _start(void){__asm__ volatile("mov x0, sp\nbl probe_main\nmov x8, #94\nsvc #0\nbrk #0");}
