/* Ordinary Linux ELF: heap semantics, native TLS/x18 and blocking/signal control. */
typedef unsigned long u64;
static long call(u64 nr,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f)
{
    register u64 x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
    register u64 x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    register u64 x8 __asm__("x8")=nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8) : "memory","cc");
    return (long)x0;
}
#define SC(n,a,b,c,d) call(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d),0,0)
#define CHECK(e) do { if(!(e))return stage; } while(0)
static u64 tls[8];
static int tls_valid(void)
{
    u64 pointer,cookie;
    __asm__ volatile("mrs %0,tpidr_el0\nmov %1,x18":"=r"(pointer),"=r"(cookie));
    return pointer==(u64)tls && cookie==0x137;
}
long probe_main(void)
{
    int stage=110;
    u64 base=SC(214,0,0,0,0), current=base+16384+5;
    CHECK(base && (long)base>0);
    CHECK(SC(214,current,0,0,0)==(long)current);
    unsigned char *heap=(unsigned char *)base;
    CHECK(heap[0]==0 && heap[16384]==0);
    heap[0]=0x5a;heap[16384]=0x33;
    stage=111;
    u64 blocker=base+32768;
    CHECK(call(222,blocker,16384,3,0x32,-1,0)==(long)blocker);
    *(unsigned char *)blocker=0x77;
    CHECK(SC(214,base+49153,0,0,0)==(long)current);
    CHECK(*(unsigned char *)blocker==0x77 && heap[0]==0x5a);
    CHECK(SC(215,blocker,16384,0,0)==0);
    CHECK(SC(214,base+49153,0,0,0)==(long)(base+49153));
    CHECK(heap[32768]==0 && heap[49152]==0 && heap[16384]==0x33);
    stage=112;
    CHECK(SC(214,base+9,0,0,0)==(long)(base+9));
    CHECK(heap[0]==0x5a && SC(64,1,base+16384,1,0)==-14);
    CHECK(SC(214,base+32769,0,0,0)==(long)(base+32769));
    CHECK(heap[16384]==0 && heap[32768]==0 && heap[0]==0x5a);
    CHECK(SC(214,base-1,0,0,0)==(long)(base+32769));
    CHECK(SC(214,~0UL,0,0,0)==(long)(base+32769));
    CHECK(SC(214,base,0,0,0)==(long)base);
    CHECK(SC(64,1,base,1,0)==-14);
    stage=113;
    unsigned char random[16];
    CHECK(SC(278,random,16,0,0)==16);
    CHECK(SC(278,1,0,0,0)==0 && SC(278,1,16,0,0)==-14);
    CHECK(SC(278,random,1,8,0)==-22);
    u64 limit[2];CHECK(SC(261,0,3,0,limit)==0 && limit[0] && limit[0]<=limit[1]);
    stage=114;
    char name[16];
    CHECK(SC(167,15,"rdxnu-startup",0,0)==0);
    CHECK(SC(167,16,name,0,0)==0 && name[0]=='r' && name[13]==0);
    u64 pid=SC(172,0,0,0,0);
    int tid=0;
    CHECK(SC(178,0,0,0,0)==(long)pid && SC(96,&tid,0,0,0)==(long)pid);
    stage=115;
    struct action {u64 handler,flags,restorer,mask;} ignore={1,0x10000000,0,0},previous;
    CHECK(SC(134,10,&ignore,&previous,8)==0);
    CHECK(SC(134,10,0,&previous,8)==0 && previous.handler==1);
    u64 mask=1UL<<11,old=0,current_mask=0;
    CHECK(SC(135,0,&mask,&old,8)==0);
    CHECK(SC(135,0,0,&current_mask,8)==0 && (current_mask & mask));
    CHECK(SC(135,2,&old,0,8)==0);
    stage=116;
    /* The fixture is compiled with -ffixed-x18 so C never allocates this cookie. */
    __asm__ volatile("msr tpidr_el0,%0\nmov x18,#0x137"::"r"(tls):"memory");
    CHECK(tls_valid());
    for(unsigned i=0;i<1024;i++)CHECK(SC(172,0,0,0,0)==(long)pid && tls_valid());
    static const char ready[]="STARTUP_READY\n";
    CHECK(SC(64,1,ready,sizeof(ready)-1,0)==sizeof(ready)-1 && tls_valid());
    stage=117;
    char byte=0;
    CHECK(SC(63,3,&byte,1,0)==1 && byte=='T' && tls_valid());
    static const char pass[]="LINUX_STARTUP_PASS\n";
    stage=118;
    CHECK(SC(64,1,pass,sizeof(pass)-1,0)==sizeof(pass)-1 && tls_valid());
    return 0;
}
__asm__(".text\n.global _start\n_start:\nbl probe_main\nmov x8,#94\nsvc #0\nb .\n");
