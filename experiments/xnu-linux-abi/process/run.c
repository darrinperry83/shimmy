/* Native parent holds both Linux processes in a blocking wait/read handshake. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <grp.h>
static pid_t child;
static void timeout(int sig){(void)sig;if(child>0)kill(-child,SIGKILL);_exit(124);}
int main(int argc,char **argv){
 if(argc!=5)return 2;
 signal(SIGALRM,timeout);alarm(25);
 int native_code=29;
#ifdef __APPLE__
 native_code=0x123425; /* Darwin waitid retains the full value. */
#endif
 pid_t control=fork();if(control<0)return 2;if(!control)_exit(native_code);
 siginfo_t native_info={0};int control_status=0;
 if(waitid(P_PID,control,&native_info,WEXITED|WNOWAIT)||native_info.si_pid!=control||native_info.si_signo!=SIGCHLD||native_info.si_code!=CLD_EXITED||native_info.si_status!=native_code||waitpid(control,&control_status,0)!=control||!WIFEXITED(control_status)||WEXITSTATUS(control_status)!=(native_code&255))return 1;
 puts("NATIVE_WAITID_CONTROL_PASS");fflush(stdout);
 int input[2],output[2];if(pipe(input)||pipe(output))return 2;
 pid_t parent=getpid();child=fork();if(child<0)return 2;
 if(!child){
  struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
  if(setpgid(0,0))_exit(100);
  close(input[1]);close(output[0]);
  if(dup2(output[1],1)<0||dup2(input[0],3)<0)_exit(100);
  if(input[0]>3)close(input[0]);
  if(output[1]>3)close(output[1]);
  if(geteuid()==0 && (setgroups(0,NULL)||setgid(65534)||setuid(65534)))_exit(102);
  char *args[]={argv[1],argv[2],argv[3],argv[4],NULL},*env[]={NULL};execve(argv[1],args,env);perror("process exec");_exit(101);
 }
 close(input[0]);close(output[1]);char buffer[512]={0};size_t used=0;int released=0;
 while(used<sizeof(buffer)-1){
  ssize_t n=read(output[0],buffer+used,sizeof(buffer)-1-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;
  if(!released&&strstr(buffer,"CHILD_READY\n")&&strstr(buffer,"PARENT_WAIT\n")){
   usleep(200000);int status=0;
   if(waitpid(child,&status,WNOHANG)!=0||write(input[1],"T",1)!=1)return 1;
   released=1;close(input[1]);
  }
 }
 if(!released)close(input[1]);
 close(output[0]);int status=0;while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 2;alarm(0);
 printf("LINUX_PROCESS_RESULT status=%d native_parent=%d output=%s",status,getpid(),buffer);
 const char *a="CHILD_READY\nPARENT_WAIT\nARCH_CHILD_EXEC_PASS\nLINUX_PROCESS_PASS\n",*b="PARENT_WAIT\nCHILD_READY\nARCH_CHILD_EXEC_PASS\nLINUX_PROCESS_PASS\n";
 if(parent!=getpid()||!WIFEXITED(status)||WEXITSTATUS(status)||!released||(strcmp(buffer,a)&&strcmp(buffer,b)))return 1;
 puts("XNU_PROCESS_PERSONALITY_PASS: native fork/COW, heap/TLS, blocking waitid, WNOHANG/WNOWAIT, exact reap/signal status and unchanged Arch child exec; native parent intact");return 0;
}
