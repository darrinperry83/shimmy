/* Same dynamic ELF on Linux and XNU: kernel PT_INTERP, auxv, TLS and exec. */
typedef unsigned long u64;
static long call(u64 n,u64 a,u64 b,u64 c,u64 d){
 register u64 x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=0,x5 __asm__("x5")=0,x8 __asm__("x8")=n;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");return (long)x0;
}
#define SC(n,a,b,c,d) call(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d))
#define CHECK(e) do{if(!(e)){static const char msg[]="INTERP_CHECK " #e "\n";SC(64,2,msg,sizeof(msg)-1,0);return 110;}}while(0)
static int eq(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static __thread volatile int tls=0x1234;
extern void _start(void);
struct info{int signo,error,code,pad,pid;unsigned uid;int status,pad2;long times[2];u64 spare[10];};
long probe_main(u64 *stack){
 CHECK(stack[0]==2 && eq((const char *)stack[1],"rdxnu-custom-argv0"));
 const char *outside=(const char *)stack[2];
 u64 *env=stack+4;CHECK(*env && eq((const char *)*env,"RDXNU_INTERP_ENV=preserved") && !env[1]);
 u64 *aux=env+2,base=0,entry=0,phdr=0,phnum=0,execfn=0,page=0;
 for(unsigned n=0;n<64&&aux[0];n++,aux+=2){
  if(aux[0]==3)phdr=aux[1];if(aux[0]==5)phnum=aux[1];if(aux[0]==6)page=aux[1];
  if(aux[0]==7)base=aux[1];if(aux[0]==9)entry=aux[1];if(aux[0]==31)execfn=aux[1];
 }
 CHECK(base && phdr && phnum && entry==(u64)_start && base!=entry);
 CHECK(*(unsigned *)(void *)base==0x464c457fU && eq((const char *)execfn,"/usr/bin/interp-probe"));
 CHECK(page==4096 || page==16384);
 CHECK(tls==0x1234);tls=0x5678;CHECK(tls==0x5678);
 CHECK(SC(56,-100,outside,0,0)==-2);
 CHECK(SC(56,-100,"/../rdxnu-outside-root",0,0)==-2);
 const char *badargs[]={"negative-control",0},*badenv[]={0};
 CHECK(SC(221,"/usr/bin/missing-probe",badargs,badenv,0)==-2 && tls==0x5678);
 CHECK(SC(221,"/usr/bin/denied-probe",badargs,badenv,0)==-13 && tls==0x5678);
 CHECK(SC(221,"/usr/bin/broken-probe",badargs,badenv,0)==-5 && tls==0x5678);
 long pid=SC(220,17,0,0,0);CHECK(pid>=0);
 if(!pid){
  const char *args[]={"printf","ARCH_DIRECT_CHILD_PASS\\n",0},*empty[]={0};
  CHECK(SC(221,"/usr/bin/printf",args,empty,0)<0);return 111;
 }
 struct info info={0};CHECK(SC(95,1,pid,&info,4)==0 && info.pid==pid && info.code==1 && info.status==0);
 static const char msg[]="LINUX_INTERP_PASS\n";CHECK(SC(64,1,msg,sizeof(msg)-1,0)==sizeof(msg)-1);return 0;
}
__asm__(".text\n.global _start\n_start:\nmov x0,sp\nbl probe_main\nmov x8,#94\nsvc #0\nb .\n");
