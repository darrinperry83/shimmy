/* Freestanding Linux AArch64 ELF. Same bytes run on reference Linux and XNU. */
typedef unsigned long u64;
typedef unsigned int u32;
struct linux_stat {
    u64 dev, ino;
    u32 mode, nlink, uid, gid;
    u64 rdev, pad1;
    long size;
    int blksize, pad2;
    long blocks, atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec;
    u32 unused[2];
};
_Static_assert(sizeof(struct linux_stat)==128,"stat layout");
static long call(u64 nr,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f)
{
    register u64 x0 __asm__("x0")=a, x1 __asm__("x1")=b, x2 __asm__("x2")=c;
    register u64 x3 __asm__("x3")=d, x4 __asm__("x4")=e, x5 __asm__("x5")=f;
    register u64 x8 __asm__("x8")=nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8) : "memory","cc");
    return (long)x0;
}
#define SC(n,a,b,c,d) call(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d),0,0)
#define CHECK(expression) do { if (!(expression)) return stage; } while (0)
static int same(const struct linux_stat *a,const struct linux_stat *b)
{
    return a->ino==b->ino && a->dev==b->dev && a->mode==b->mode && a->size==b->size && a->nlink==b->nlink;
}
long probe_main(u64 *stack)
{
    int stage=81;
    CHECK(stack[0]==2);
    const char *absolute=(const char *)stack[2];
    long fd=SC(56,-100,"data",0xa0000,0);
    CHECK(fd>=0);
    CHECK(SC(25,fd,1,0,0)==1); /* CLOEXEC, translated rather than leaked bits. */
    struct linux_stat stat, other;
    stage=82;
    CHECK(SC(80,fd,&stat,0,0)==0);
    CHECK((stat.mode & 0177777)==0100640 && stat.size==32768 && stat.nlink==1 && stat.ino);
    CHECK(!stat.pad1 && !stat.pad2 && !stat.unused[0] && !stat.unused[1]);
    CHECK(stat.atime_nsec>=0 && stat.atime_nsec<1000000000);
    CHECK(stat.mtime_nsec>=0 && stat.mtime_nsec<1000000000);
    CHECK(stat.ctime_nsec>=0 && stat.ctime_nsec<1000000000);
    stage=83;
    CHECK(SC(79,3,"data",&other,0)==0 && same(&stat,&other));
    CHECK(SC(79,-100,"data",&other,0)==0 && same(&stat,&other));
    CHECK(SC(79,-1,absolute,&other,0)==0 && same(&stat,&other));
    CHECK(SC(79,fd,"",&other,0x1000)==0 && same(&stat,&other));
    stage=84;
    char buffer[8];
    CHECK(SC(63,fd,buffer,1,0)==1 && buffer[0]==0x41);
    CHECK(SC(62,fd,32767,0,0)==32767);
    CHECK(SC(63,fd,buffer,1,0)==1 && buffer[0]==0x42);
    CHECK(SC(62,fd,0,2,0)==32768 && SC(63,fd,buffer,1,0)==0);
    CHECK(SC(62,fd,0x100000001UL,0,0)==0x100000001L);
    stage=85;
    long mapping=call(222,0,16384,5,2,fd,16384);
    CHECK(mapping>0);
    CHECK(SC(57,fd,0,0,0)==0);
    CHECK(((int (*)(void))(u64)mapping)()==37); /* Mapping survives close. */
    CHECK(SC(215,mapping,16384,0,0)==0);
    CHECK(SC(57,fd,0,0,0)==-9);
    stage=86;
    fd=SC(56,3,"data",0x20000,0); CHECK(fd>=0);
    CHECK(SC(25,fd,1,0,0)==0);
    CHECK(SC(25,fd,2,1,0)==0 && SC(25,fd,1,0,0)==1);
    CHECK(SC(57,fd,0,0,0)==0);
    fd=SC(56,-1,absolute,0,0); CHECK(fd>=0);
    CHECK(SC(57,fd,0,0,0)==0);
    stage=87;
    CHECK(SC(79,3,"link",&other,0)==0 && same(&stat,&other));
    CHECK(SC(79,3,"link",&other,0x100)==0 && (other.mode & 0170000)==0120000 && other.size==4);
    CHECK(SC(78,3,"link",buffer,sizeof(buffer))==4);
    CHECK(buffer[0]=='d' && buffer[1]=='a' && buffer[2]=='t' && buffer[3]=='a');
    CHECK(SC(78,3,"link",buffer,2)==2);
    CHECK(SC(56,3,"link",0x8000,0)==-40);
    stage=88;
    fd=SC(56,3,".",0x4000,0); CHECK(fd>=0);
    CHECK(SC(80,fd,&other,0,0)==0 && (other.mode & 0170000)==0040000);
    CHECK(SC(57,fd,0,0,0)==0);
    CHECK(SC(56,3,"data",0x4000,0)==-20);
    CHECK(SC(56,-1,"data",0,0)==-9);
    CHECK(SC(56,3,"absent",0,0)==-2);
    CHECK(SC(56,3,1,0,0)==-14);
    stage=89;
    CHECK(SC(80,-1,&other,0,0)==-9);
    stage=101;
    CHECK(SC(80,3,1,0,0)==-14);
    stage=102;
    CHECK(SC(79,3,1,&other,0)==-14);
    stage=103;
    CHECK(SC(79,3,"data",1,0)==-14);
    stage=104;
    CHECK(SC(79,3,"data",&other,0x80000000UL)==-22);
    stage=105;
    CHECK(SC(78,3,"link",1,8)==-14);
    stage=106;
    CHECK(SC(78,3,"link",buffer,0)==-22);
    stage=90;
    fd=SC(56,3,"data",0,0); CHECK(fd>=0);
    CHECK(SC(62,fd,-1,0,0)==-22);
    CHECK(SC(57,fd,0,0,0)==0);
    CHECK(SC(62,-1,0,0,0)==-9);
    stage=91;
    CHECK(SC(64,1,&stat,sizeof(stat),0)==sizeof(stat));
    static const char message[]="LINUX_FILES_PASS\n";
    CHECK(SC(64,1,message,sizeof(message)-1,0)==sizeof(message)-1);
    return 0;
}
__asm__(".text\n.global _start\n_start:\nmov x0,sp\nbl probe_main\nmov x8,#94\nsvc #0\nb .\n");
