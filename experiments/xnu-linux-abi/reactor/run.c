/* Native supervisor forces peer/epoll and blocked signalfd wakeups. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
static pid_t child;
static void timeout(int sig){(void)sig;if(child>0)kill(-child,SIGKILL);_exit(124);}
int main(int argc,char **argv){
 if(argc!=2)return 2;signal(SIGALRM,timeout);alarm(40);int input[2],output[2];if(pipe(input)||pipe(output))return 2;
 pid_t parent=getpid();child=fork();if(child<0)return 2;
 if(!child){struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);if(setpgid(0,0))_exit(120);
  close(input[1]);close(output[0]);if(dup2(output[1],1)<0||dup2(input[0],3)<0)_exit(120);
  if(input[0]>3)close(input[0]);if(output[1]>3)close(output[1]);
  if(geteuid()==0&&(setgroups(0,NULL)||setgid(65534)||setuid(65534)))_exit(120);
  char *args[]={argv[1],NULL},*env[]={NULL};execve(argv[1],args,env);perror("reactor exec");_exit(121);}
 close(input[0]);close(output[1]);char buffer[4096]={0};size_t used=0;int released=0;
 const char *waits[]={"REACTOR_EPOLL_WAIT\n","REACTOR_SIGNAL_WAIT\n"},*ready[]={"REACTOR_CHILD_READY\n","REACTOR_SIGNAL_CHILD_READY\n"};
 while(used<sizeof buffer-1){ssize_t n=read(output[0],buffer+used,sizeof buffer-1-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=n;
  if(released<2&&strstr(buffer,waits[released])&&strstr(buffer,ready[released])){usleep(200000);int status=0;
   if(waitpid(child,&status,WNOHANG)!=0||write(input[1],"T",1)!=1)return 1;released++;}}
 close(input[1]);close(output[0]);int status=0;while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 2;
 printf("LINUX_REACTOR_RESULT status=%d native_parent=%d releases=%d output=%s",status,getpid(),released,buffer);
 if(parent!=getpid()||!WIFEXITED(status)||WEXITSTATUS(status)||released!=2||!strstr(buffer,"LINUX_EPOLL_PASS\n")||!strstr(buffer,"LINUX_TIMERFD_PASS\n")||!strstr(buffer,"LINUX_SIGNALFD_PASS\n")||!strstr(buffer,"LINUX_REACTOR_PASS\n"))return 1;
#ifdef __APPLE__
 child=fork();if(child<0)return 2;if(!child){char *args[]={argv[1],"constraints",NULL},*env[]={NULL};execve(argv[1],args,env);_exit(121);}
 while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 2;
 if(!WIFEXITED(status)||WEXITSTATUS(status))return 1;
#endif
 alarm(0);puts("XNU_REACTOR_PERSONALITY_PASS: native epoll, scheduled timers, pending-signal reads, service fork/exec and two forced peer wakeups; native parent intact");return 0;
}
