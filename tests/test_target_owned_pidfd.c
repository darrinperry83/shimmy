/* Native target-owned pidfd integration test; no guest root or live sockets. */
#include "../src/pidfd_shim.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
int linux_errno_from_host(int e) { return e == ETIMEDOUT ? 110 : e; }
#include <sys/stat.h>
#include <sys/proc_info.h>
#include <libproc.h>
#include <sys/un.h>
#include <dirent.h>

/* A small native model of Shimmy's high-fd reservation/close_range rule. */
static int keep[1024], nkeep;
int shim_fd_reserve(int fd) {
  if(fd<256){int hi=fcntl(fd,F_DUPFD_CLOEXEC,256);if(hi<0)return -1;close(fd);fd=hi;}
  for(int i=0;i<nkeep;i++)if(keep[i]==fd)return fd;
  if(nkeep>=1024)return -1;
  keep[nkeep++]=fd;(void)fcntl(fd,F_SETFD,FD_CLOEXEC);return fd;
}
void shim_fd_unreserve(int fd) { for(int i=0;i<nkeep;i++)if(keep[i]==fd){keep[i]=keep[--nkeep];break;} }
static void simulate_close_range(void){
  for(int fd=3;fd<512;fd++){
    int reserved=0;for(int i=0;i<nkeep;i++)if(keep[i]==fd)reserved=1;
    if(!reserved)close(fd);
  }
}
static int send_fd(int sock,int fd){
  char x='x',ctl[CMSG_SPACE(sizeof(int))]={0}; struct iovec iov={&x,1};
  struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=ctl,.msg_controllen=sizeof ctl};
  struct cmsghdr*c=CMSG_FIRSTHDR(&m);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;
  c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&fd,sizeof fd);
  return sendmsg(sock,&m,0)==1?0:-1;
}
static int recv_fd(int sock){
  char x,ctl[CMSG_SPACE(sizeof(int))]={0}; struct iovec iov={&x,1};
  struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=ctl,.msg_controllen=sizeof ctl};
  if(recvmsg(sock,&m,0)!=1)return -1;
  struct cmsghdr*c=CMSG_FIRSTHDR(&m);if(!c||c->cmsg_level!=SOL_SOCKET||c->cmsg_type!=SCM_RIGHTS||c->cmsg_len!=CMSG_LEN(sizeof(int)))return -1;
  int fd;memcpy(&fd,CMSG_DATA(c),sizeof fd);return fd;
}
static int forged_birth_rejected(const char* dir,pid_t target){
  struct proc_bsdinfo b={0};
  if(proc_pidinfo(target,PROC_PIDTBSDINFO,0,&b,sizeof b)!=sizeof b)return 0;
  struct sockaddr_un a={.sun_family=AF_UNIX};
  snprintf(a.sun_path,sizeof a.sun_path,"%s/pfd-data-%d-%llu-%llu-%08x",dir,target,
           (unsigned long long)b.pbi_start_tvsec,
           (unsigned long long)(b.pbi_start_tvusec+1),1234);
  int l=socket(AF_UNIX,SOCK_STREAM,0),w=socket(AF_UNIX,SOCK_STREAM,0);
  if(l<0||w<0||bind(l,(void*)&a,sizeof a)||listen(l,1)||connect(w,(void*)&a,sizeof a))return 0;
  int r=accept(l,0,0);unlink(a.sun_path);
  int rejected=r>=0&&pidfd_shim_pid(r)==0;
  close(r);close(w);close(l);return rejected;
}
static void cleanup_dir(const char*dir){
  DIR*d=opendir(dir);if(!d)return;
  struct dirent*e;while((e=readdir(d)))if(e->d_name[0]!='.'){
    char p[256];snprintf(p,sizeof p,"%s/%s",dir,e->d_name);unlink(p);
  }
  closedir(d);rmdir(dir);
}
int main(int argc,char**argv){
  if(argc==3 && strcmp(argv[1],"--target-exec")==0){
    if(pidfd_shim_target_init()<0)return 21;
    if(write(1,"E",1)!=1)return 22;
    simulate_close_range(); /* guest close_range must retain writer + listener */
    char c;if(read(0,&c,1)!=1||c!='X')return 23;
    return 0;
  }
  char dir[]="/tmp/rdpidfd-test-XXXXXX";if(!mkdtemp(dir))return 1;
  if(setenv("RDSOCKETGROUP",dir,1)||setenv("RDPIDFD_SOCKET","1",1))return 2;
  int ready[2],stop[2],xfer[2];
  if(pipe(ready)||pipe(stop)||socketpair(AF_UNIX,SOCK_STREAM,0,xfer))return 3;
  pid_t target=fork();if(target<0)return 4;
  if(target==0){
    close(ready[0]);close(stop[1]);close(xfer[0]);close(xfer[1]);
    if(pidfd_shim_target_init()<0)_exit(5);
    if(write(ready[1],"R",1)!=1)_exit(6);
    char c;if(read(stop[0],&c,1)!=1||c!='E')_exit(7);
    if(dup2(stop[0],0)<0||dup2(ready[1],1)<0)_exit(8);
    char*args[]={argv[0],"--target-exec","unused",NULL};execv(argv[0],args);_exit(9);
  }
  close(ready[1]);close(stop[0]);char c;if(read(ready[0],&c,1)!=1)return 8;
  pid_t creator=fork();if(creator<0)return 9;
  if(creator==0){
    close(xfer[0]);
    if(pidfd_shim_target_init()<0)_exit(10);
    int fd=(int)pidfd_open_shim(target,0);if(fd<0)_exit(11);
    if(send_fd(xfer[1],fd)<0)_exit(12);
    _exit(0);
  }
  close(xfer[1]);int fd=recv_fd(xfer[0]);if(fd<0)return 13;
  int status;if(waitpid(creator,&status,0)!=creator||!WIFEXITED(status)||WEXITSTATUS(status))return 14;
  if(pidfd_shim_pid(fd)!=target)return 15;
  if(pidfd_shim_send_signal(fd,0)!=0)return 30;
  errno=0;if(pidfd_shim_send_signal(fd,SIGTERM)!=-1||errno!=ENOTSUP)return 31;
  if(pidfd_shim_pid(xfer[0])!=0)return 24; /* unrelated socket */
  if(!forged_birth_rejected(dir,target))return 28;
  struct pollfd p={.fd=fd,.events=POLLIN|POLLHUP};
  if(poll(&p,1,0)!=0)return 16; /* creator dead, target alive */
  if(write(stop[1],"E",1)!=1)return 17;
  if(read(ready[0],&c,1)!=1||c!='E')return 25; /* survived guest exec */
  if(poll(&p,1,0)!=0)return 26;
  if(write(stop[1],"X",1)!=1)return 27;
  if(waitpid(target,&status,0)!=target||!WIFEXITED(status)||WEXITSTATUS(status))return 18;
  if(pidfd_shim_pid(fd)!=target){int p=0;socklen_t n=sizeof p;int q=getsockopt(fd,SOL_LOCAL,LOCAL_PEERPID,&p,&n);struct sockaddr_un a={0};socklen_t z=sizeof a;int g=getsockname(fd,(void*)&a,&z);struct stat st={0};int f=fstat(fd,&st);fprintf(stderr,"post-exit peer=%d query=%d errno=%d getname=%d name=%s fstat=%d dev=%llu ino=%llu pidfdpid=%d target=%d\n",p,q,errno,g,a.sun_path,f,(unsigned long long)st.st_dev,(unsigned long long)st.st_ino,pidfd_shim_pid(fd),target);return 32;} /* identity survives target exit */
  errno=0;if(pidfd_shim_send_signal(fd,0)!=-1||errno!=ESRCH)return 33;
  if(poll(&p,1,1000)!=1||!(p.revents&(POLLIN|POLLHUP)))return 19;
  if(read(fd,&c,1)!=0)return 20;
  int reused=fd;
  close(fd);
  if(dup2(xfer[0],reused)!=reused||pidfd_shim_pid(reused)!=0)return 34;
  close(reused);
  if(xfer[0]!=reused)close(xfer[0]);close(ready[0]);close(stop[1]);
  if(chmod(dir,0755)<0||pidfd_open_shim(getpid(),0)!=-EACCES)return 29;
  chmod(dir,0700);cleanup_dir(dir);
  puts("TARGET_OWNED_PIDFD_PASS");return 0;
}
