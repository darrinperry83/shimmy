/* Native supervisor verifies that each Linux blocking path really waits. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <grp.h>
#ifdef __APPLE__
#include <sys/event.h>
#endif
static pid_t child;
static void timeout(int sig){(void)sig;if(child>0)kill(-child,SIGKILL);_exit(124);}
static int native_control(void){
 int fds[2];if(pipe(fds))return 1;
#ifdef __APPLE__
 int queue=kqueue();struct kevent change,event;struct timespec zero={0,0};
 if(queue<0)return 1;
 EV_SET(&change,fds[0],EVFILT_READ,EV_ADD,0,0,(void *)0x1234);
 if(kevent(queue,&change,1,&event,1,&zero)!=0)return 1;
#endif
 if(write(fds[1],"N",1)!=1)return 1;
 struct pollfd f={fds[0],POLLIN,0};if(poll(&f,1,0)!=1||f.revents!=POLLIN)return 1;
#ifdef __APPLE__
 if(kevent(queue,NULL,0,&event,1,&zero)!=1||event.udata!=(void *)0x1234||event.data!=1)return 1;
 close(queue);
#endif
 char c=0;if(read(fds[0],&c,1)!=1||c!='N')return 1;close(fds[0]);close(fds[1]);return 0;
}
int main(int argc,char **argv){
 if(argc!=2)return 2;if(native_control())return 1;
 puts("NATIVE_EVENT_CONTROL_PASS");fflush(stdout);
 signal(SIGALRM,timeout);alarm(30);int input[2],output[2];if(pipe(input)||pipe(output))return 2;
 pid_t parent=getpid();child=fork();if(child<0)return 2;
 if(!child){
  struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);if(setpgid(0,0))_exit(120);
  close(input[1]);close(output[0]);if(dup2(output[1],1)<0||dup2(input[0],3)<0)_exit(120);
  if(input[0]>3)close(input[0]);if(output[1]>3)close(output[1]);
  if(geteuid()==0&&(setgroups(0,NULL)||setgid(65534)||setuid(65534)))_exit(120);
  char *args[]={argv[1],NULL},*env[]={NULL};execve(argv[1],args,env);perror("events exec");_exit(121);
 }
 close(input[0]);close(output[1]);char buffer[2048]={0};size_t used=0;int released=0;
 while(used<sizeof buffer-1){
  ssize_t n=read(output[0],buffer+used,sizeof buffer-1-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=n;
  if(released<3){
   char phase[32],ready[32];snprintf(phase,sizeof phase,"EVENT_PHASE_%d\n",released+1);snprintf(ready,sizeof ready,"EVENT_CHILD_%d\n",released+1);
   if(strstr(buffer,phase)&&strstr(buffer,ready)){
    usleep(200000);int status=0;if(waitpid(child,&status,WNOHANG)!=0||write(input[1],"T",1)!=1)return 1;released++;
   }
  }
 }
 close(input[1]);close(output[0]);int status=0;while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 2;alarm(0);
 printf("LINUX_EVENTS_RESULT status=%d native_parent=%d releases=%d output=%s",status,getpid(),released,buffer);
 if(parent!=getpid()||!WIFEXITED(status)||WEXITSTATUS(status)||released!=3||!strstr(buffer,"EVENTS_EXEC_FD_PASS\nLINUX_EVENTS_PASS\n"))return 1;
 puts("XNU_EVENTS_PERSONALITY_PASS: counter/semaphore, copy faults, dup/fork/exec lifetime, readiness, timeout and three forced blocking wakeups; native parent intact");return 0;
}
