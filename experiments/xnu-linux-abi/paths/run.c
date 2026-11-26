/* Native parent owns fixtures and races pathname replacement against held Linux references. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <unistd.h>
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
struct linux_stat {uint64_t dev,ino;uint32_t mode,nlink,uid,gid;uint64_t rdev,pad1;int64_t size;int32_t blksize,pad2;int64_t blocks,as,an,ms,mn,cs,cn;uint32_t spare[2];};
static pid_t child;
static void timeout(int sig){(void)sig;if(child>0)kill(child,SIGKILL);_exit(124);}
static int seed(int dir,const char *path,mode_t mode){int f=openat(dir,path,O_CREAT|O_EXCL|O_WRONLY,0600);if(f<0)return -1;int ok=write(f,"payload",7)==7 && fchmod(f,mode)==0;close(f);return ok?0:-1;}
int main(int argc,char **argv){
 if(argc!=2 || geteuid()!=0){fprintf(stderr,"paths fixture requires a root native parent\n");return 2;}
 char executable[PATH_MAX];if(!realpath(argv[1],executable))return 2;
 signal(SIGALRM,timeout);alarm(25);pid_t parent=getpid();
 char directory[]="/tmp/rdxnu-paths-XXXXXX";if(!mkdtemp(directory)||chmod(directory,0755))return 2;
 int dir=open(directory,O_RDONLY|O_DIRECTORY);if(dir<0)return 2;
 if(seed(dir,"data",0644)||seed(dir,"unreadable",0000)||symlinkat("data",dir,"link")||symlinkat("missing",dir,"dangling")||mkdirat(dir,"sub",0755)||mkdirat(dir,"blocked",0700)||seed(dir,"blocked/leaf",0644)||fchmodat(dir,"blocked",0000,0)||mkfifoat(dir,"fifo",0000))return 2;
 struct stat expected;if(fstatat(dir,"data",&expected,0))return 2;
 int input[2],output[2];if(pipe(input)||pipe(output))return 2;
 child=fork();if(child<0)return 2;
 if(!child){
  struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
  close(input[1]);close(output[0]);
  if(dup2(output[1],1)<0||fchdir(dir)||dup2(dir,3)<0||dup2(input[0],4)<0)_exit(100);
  if(output[1]>4)close(output[1]);
  if(input[0]>4)close(input[0]);
  if(dir>4)close(dir);
  if(setgroups(0,NULL)||setgid(65534)||setuid(65534))_exit(102);
  char *args[]={executable,NULL},*env[]={NULL};execve(executable,args,env);perror("paths ELF exec");_exit(101);
 }
 close(input[0]);close(output[1]);unsigned char bytes[512]={0};size_t used=0;int released=0;
 while(used<sizeof(bytes)){
  ssize_t n=read(output[0],bytes+used,sizeof(bytes)-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;
  if(!released&&used>=128+sizeof("PATH_READY\n")-1){
   if(memcmp(bytes+128,"PATH_READY\n",sizeof("PATH_READY\n")-1))return 1;
   if(unlinkat(dir,"link",0)||symlinkat("unreadable",dir,"link")||unlinkat(dir,"data",0)||seed(dir,"data",0644)||write(input[1],"T",1)!=1)return 2;
   close(input[1]);released=1;
  }
 }
 if(!released)close(input[1]);
 close(output[0]);int status=0;while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 2;alarm(0);
 fchmodat(dir,"blocked",0700,0);unlinkat(dir,"blocked/leaf",0);unlinkat(dir,"blocked",AT_REMOVEDIR);unlinkat(dir,"sub",AT_REMOVEDIR);
 const char *names[]={"data","unreadable","link","dangling","fifo"};for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)unlinkat(dir,names[i],0);
 struct stat absent;int created=fstatat(dir,"not-created",&absent,0)==0;if(created)unlinkat(dir,"not-created",0);close(dir);rmdir(directory);
 printf("LINUX_PATHS_RESULT status=%d native_parent=%d bytes=%zu\n",status,getpid(),used);
 if(!WIFEXITED(status)||WEXITSTATUS(status)||parent!=getpid()||created||used!=128+sizeof("PATH_READY\nLINUX_PATHS_PASS\n")-1)return 1;
 struct linux_stat got;memcpy(&got,bytes,sizeof(got));
 uint64_t device=((uint64_t)minor(expected.st_dev)&255)|(((uint64_t)major(expected.st_dev)&4095)<<8)|(((uint64_t)minor(expected.st_dev)&~255ULL)<<12)|(((uint64_t)major(expected.st_dev)&~4095ULL)<<32);
 if(got.dev!=device||got.ino!=expected.st_ino||got.mode!=expected.st_mode||got.nlink!=expected.st_nlink||got.uid!=expected.st_uid||got.gid!=expected.st_gid||got.size!=expected.st_size||memcmp(bytes+128,"PATH_READY\nLINUX_PATHS_PASS\n",sizeof("PATH_READY\nLINUX_PATHS_PASS\n")-1))return 1;
 puts("XNU_PATHS_PERSONALITY_PASS: O_PATH permissions/no data I/O, directory lookup, held/unlinked symlinks and inodes, dup/close, statx; native parent intact");return 0;
}
