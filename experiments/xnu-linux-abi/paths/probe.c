/* Unchanged Linux ELF: native path-only references, permissions and metadata. */
typedef unsigned long u64;
static long call(u64 n,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f){
 register u64 x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f,x8 __asm__("x8")=n;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");return (long)x0;
}
#define SC(n,a,b,c,d) call(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d),0,0)
#define CHECK(e) do{if(!(e)){static const char failure[]="PATH_CHECK " #e "\n";SC(64,2,failure,sizeof(failure)-1,0);return stage;}}while(0)
struct stat64 {u64 dev,ino;unsigned mode,nlink,uid,gid;u64 rdev,pad1;long size;int blksize,pad2;long blocks,as,an,ms,mn,cs,cn;unsigned spare[2];};
struct stamp{long sec;unsigned nsec;int reserved;};
struct statx {unsigned mask,blksize;u64 attributes;unsigned nlink,uid,gid;unsigned short mode,pad;u64 ino,size,blocks,attributes_mask;struct stamp atime,btime,ctime,mtime;unsigned rmaj,rmin,dmaj,dmin;u64 mount;unsigned dio_mem,dio_off;u64 spare[12];};
_Static_assert(sizeof(struct stat64)==128,"stat size");
_Static_assert(sizeof(struct statx)==256,"statx size");
static int same(struct stat64 *s,struct statx *x){return s->ino==x->ino&&s->mode==x->mode&&s->nlink==x->nlink&&s->uid==x->uid&&s->gid==x->gid&&s->size==(long)x->size&&s->blocks==(long)x->blocks;}
long probe_main(void){
 int stage=120;struct stat64 original,other,linkstat;struct statx sx;char buffer[32]={0};
 CHECK(SC(174,0,0,0,0)==65534);
 long fd=SC(56,3,"data",0x200000,0);CHECK(fd>=0);
 CHECK(SC(25,fd,3,0,0)==0x200000 && SC(25,fd,1,0,0)==0);
 CHECK(SC(80,fd,&original,0,0)==0);
 CHECK(SC(63,fd,buffer,1,0)==-9 && SC(64,fd,buffer,1,0)==-9 && SC(62,fd,0,0,0)==-9);
 CHECK(call(222,0,16384,1,2,fd,0)==-9 && SC(25,fd,4,0,0)==-9);
 CHECK(call(291,fd,(u64)"",0x1000,0x5109,(u64)&sx,0)==0 && (sx.mask&0x109)==0x109 && same(&original,&sx));
 CHECK(!(sx.mask&0x5000) ? sx.mount==0 : sx.mount!=0);
 u64 mount=sx.mount;unsigned mount_mask=sx.mask&0x5000;
 CHECK(mount_mask && mount);
 CHECK(call(291,3,(u64)"data",0,0x5109,(u64)&sx,0)==0 && sx.mount==mount && (sx.mask&0x5000));
 long dev=SC(56,-100,"/dev/null",0x200000,0);CHECK(dev>=0);
 CHECK(call(291,dev,(u64)"",0x1000,0x5109,(u64)&sx,0)==0 && (sx.mask&0x5000) && sx.mount && sx.mount!=mount);
 CHECK(SC(57,dev,0,0,0)==0);
 long root=SC(56,-100,"/",0x204000,0);CHECK(root>=0);
 CHECK(call(291,root,(u64)"",0x1000,0x5109,(u64)&sx,0)==0 && (sx.mask&0x5000) && sx.mount);
 u64 root_mount=sx.mount;
 CHECK(call(291,-100,(u64)"/",0,0x5109,(u64)&sx,0)==0 && sx.mount==root_mount && (sx.mask&0x5000));
 CHECK(SC(57,root,0,0,0)==0);
 stage=121;
 long unreadable=SC(56,3,"unreadable",0x200000,0);CHECK(unreadable>=0);
 CHECK(SC(56,3,"unreadable",0,0)==-13 && SC(63,unreadable,buffer,1,0)==-9);
 CHECK(SC(80,unreadable,&other,0,0)==0 && !(other.mode&0777));
 CHECK(SC(57,unreadable,0,0,0)==0);
 CHECK(SC(56,3,"blocked/leaf",0x200000,0)==-13);
 long blocked=SC(56,3,"blocked",0x204000,0);CHECK(blocked>=0);
 CHECK(SC(56,blocked,"leaf",0x200000,0)==-13 && SC(57,blocked,0,0,0)==0);
 long fifo=SC(56,3,"fifo",0x200000,0);CHECK(fifo>=0 && SC(57,fifo,0,0,0)==0);
 stage=122;
 long dir=SC(56,3,"sub",0x284000,0);CHECK(dir>=0);
 CHECK(SC(25,dir,3,0,0)==0x204000 && SC(25,dir,1,0,0)==1);
 long inside=SC(56,dir,"../data",0x200000,0);CHECK(inside>=0 && SC(57,inside,0,0,0)==0);
 CHECK(SC(57,dir,0,0,0)==0);
 CHECK(SC(56,3,"data",0x204000,0)==-20);
 long ignored=SC(56,3,"data",0x200241,0777);CHECK(ignored>=0 && SC(25,ignored,3,0,0)==0x200000 && SC(57,ignored,0,0,0)==0);
 CHECK(SC(56,3,"not-created",0x200040,0777)==-2);
 stage=123;
 long link=SC(56,3,"link",0x288000,0);CHECK(link>=0);
 CHECK(SC(80,link,&linkstat,0,0)==0 && (linkstat.mode&0170000)==0120000);
 CHECK(SC(78,link,"",buffer,2)==2 && buffer[0]=='d'&&buffer[1]=='a');
 CHECK(SC(78,link,"",buffer,0)==-22);
 CHECK(SC(56,3,"link",0x208000|0x4000,0)==-20);
 CHECK(SC(56,3,"dangling",0x200000,0)==-2);
 long dangling=SC(56,3,"dangling",0x208000,0);CHECK(dangling>=0 && SC(57,dangling,0,0,0)==0);
 long copy=SC(25,link,1030,30,0);CHECK(copy>=30 && SC(25,copy,1,0,0)==1);
 CHECK(SC(57,link,0,0,0)==0);link=copy;
 CHECK(SC(79,link,"",&other,0x1100)==0 && other.ino==linkstat.ino);
 stage=124;
 CHECK(call(291,-100,(u64)"",0x1000,0x7ff,(u64)&sx,0)==0 && (sx.mode&0170000)==0040000);
 CHECK(call(291,3,(u64)"data",0,0x7ff,(u64)&sx,0)==0 && same(&original,&sx));
 CHECK(call(291,-1,(u64)"",0x1000,0x7ff,(u64)&sx,0)==-9);
 CHECK(call(291,fd,(u64)"",0x1000,0x7ff,1,0)==-14);
 CHECK(call(291,3,1,0,0x7ff,(u64)&sx,0)==-14);
 CHECK(call(291,3,(u64)"data",0x6000,0x7ff,(u64)&sx,0)==-22);
 CHECK(call(291,3,(u64)"data",0,0x80000000,(u64)&sx,0)==-22);
 CHECK(SC(78,-100,"",buffer,8)==-2 && SC(78,fd,"",buffer,8)==-2);
 CHECK(SC(64,1,&original,sizeof(original),0)==sizeof(original));
 static const char ready[]="PATH_READY\n";CHECK(SC(64,1,ready,sizeof(ready)-1,0)==sizeof(ready)-1);
 stage=125;char byte=0;CHECK(SC(63,4,&byte,1,0)==1&&byte=='T');
 CHECK(SC(80,fd,&other,0,0)==0 && other.ino==original.ino && other.nlink==0);
 CHECK(call(291,fd,(u64)"",0x1000,0x7ff,(u64)&sx,0)==0 && sx.ino==original.ino && sx.nlink==0);
 CHECK(call(291,fd,(u64)"",0x1000,0x5109,(u64)&sx,0)==0 && sx.mount==mount && (sx.mask&0x5000));
 CHECK(SC(78,link,"",buffer,32)==4 && buffer[0]=='d'&&buffer[1]=='a'&&buffer[2]=='t'&&buffer[3]=='a');
 CHECK(SC(80,link,&other,0,0)==0 && other.ino==linkstat.ino && other.nlink==0);
 CHECK(call(291,3,(u64)"link",0x100,0x7ff,(u64)&sx,0)==0 && sx.ino!=linkstat.ino && (sx.mode&0170000)==0120000);
 CHECK(SC(57,link,0,0,0)==0 && SC(57,fd,0,0,0)==0);
 static const char pass[]="LINUX_PATHS_PASS\n";CHECK(SC(64,1,pass,sizeof(pass)-1,0)==sizeof(pass)-1);return 0;
}
__asm__(".text\n.global _start\n_start:\nbl probe_main\nmov x8,#94\nsvc #0\nb .\n");
