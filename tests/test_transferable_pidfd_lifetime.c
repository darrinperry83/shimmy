/* macOS primitive for a transferable pidfd: target owns peer, creator may die. */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/proc_info.h>
#include <libproc.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int birth_matches(pid_t pid, unsigned long long sec, unsigned long long usec) {
  struct proc_bsdinfo p={0};
  return proc_pidinfo(pid,PROC_PIDTBSDINFO,0,&p,sizeof p)==sizeof p &&
         p.pbi_pid==(unsigned)pid && p.pbi_start_tvsec==sec && p.pbi_start_tvusec==usec;
}

static int receive_fd(int s) {
  char byte, control[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec iov = {&byte, 1};
  struct msghdr m = {.msg_iov=&iov, .msg_iovlen=1, .msg_control=control, .msg_controllen=sizeof control};
  if (recvmsg(s, &m, 0) != 1) return -1;
  struct cmsghdr* c = CMSG_FIRSTHDR(&m);
  if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
  int fd; memcpy(&fd, CMSG_DATA(c), sizeof fd); return fd;
}
static int send_fd(int s, int fd) {
  char byte='x', control[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec iov = {&byte, 1};
  struct msghdr m = {.msg_iov=&iov, .msg_iovlen=1, .msg_control=control, .msg_controllen=sizeof control};
  struct cmsghdr* c=CMSG_FIRSTHDR(&m); c->cmsg_level=SOL_SOCKET; c->cmsg_type=SCM_RIGHTS;
  c->cmsg_len=CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(c),&fd,sizeof fd);
  return sendmsg(s,&m,0)==1 ? 0 : -1;
}
int main(void) {
  int control[2]; if (socketpair(AF_UNIX,SOCK_STREAM,0,control)<0) return 1;
  pid_t target=fork(); if(target<0) return 2;
  if(target==0) {
    close(control[0]); int writer=receive_fd(control[1]); if(writer<0) _exit(3);
    if(write(control[1],"A",1)!=1) _exit(4);
    sleep(2); close(writer); _exit(0);
  }
  close(control[1]);
  int listener=socket(AF_UNIX,SOCK_STREAM,0), writer=socket(AF_UNIX,SOCK_STREAM,0);
  if(listener<0 || writer<0) return 5;
  struct proc_bsdinfo target_info={0};
  if(proc_pidinfo(target,PROC_PIDTBSDINFO,0,&target_info,sizeof target_info)!=sizeof target_info) return 17;
  if(!birth_matches(target,target_info.pbi_start_tvsec,target_info.pbi_start_tvusec) ||
      birth_matches(target,target_info.pbi_start_tvsec,target_info.pbi_start_tvusec+1)) return 18;
  struct sockaddr_un addr={.sun_family=AF_UNIX};
  snprintf(addr.sun_path,sizeof addr.sun_path,"/tmp/rdpfd-%d-%llu-%llu",target,
           (unsigned long long)target_info.pbi_start_tvsec,
           (unsigned long long)target_info.pbi_start_tvusec);
  unlink(addr.sun_path);
  if(bind(listener,(void*)&addr,sizeof addr)<0 || listen(listener,1)<0 ||
     connect(writer,(void*)&addr,sizeof addr)<0) return 6;
  int reader=accept(listener,NULL,NULL); if(reader<0) return 7;
  close(listener); unlink(addr.sun_path);
  pid_t creator=fork(); if(creator<0) return 8;
  if(creator==0) {
    close(reader); if(send_fd(control[0],writer)<0) _exit(9);
    char ack; if(read(control[0],&ack,1)!=1 || ack!='A') _exit(10);
    _exit(0);
  }
  close(writer); close(control[0]);
  int status; if(waitpid(creator,&status,0)!=creator || !WIFEXITED(status) || WEXITSTATUS(status)) return 11;
  struct sockaddr_un found={0}; socklen_t n=sizeof found;
  if(getsockname(reader,(void*)&found,&n)<0 || strcmp(found.sun_path,addr.sun_path)) return 12;
  struct pollfd pfd={.fd=reader,.events=POLLIN|POLLHUP};
  if(poll(&pfd,1,0)!=0) return 13; /* creator died; target still owns peer */
  if(waitpid(target,&status,0)!=target || !WIFEXITED(status) || WEXITSTATUS(status)) return 14;
  if(birth_matches(target,target_info.pbi_start_tvsec,target_info.pbi_start_tvusec)) return 19;
  if(poll(&pfd,1,1000)!=1 || !(pfd.revents&(POLLIN|POLLHUP))) return 15;
  char c; if(read(reader,&c,1)!=0) return 16;
  puts("TRANSFERABLE_PIDFD_LIFETIME_PASS"); return 0;
}
