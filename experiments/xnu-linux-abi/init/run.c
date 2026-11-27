/* Native supervisor remains outside the Linux lineage and owns its children. */
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
static void timeout(int signal){(void)signal;if(child>0)kill(-child,SIGKILL);_exit(124);}
int main(int argc,char **argv){
 if(argc!=2)return 2;
 pid_t parent=getpid(),ppid=getppid();int output[2];if(pipe(output))return 2;
 signal(SIGALRM,timeout);alarm(30);fflush(NULL);child=fork();if(child<0)return 2;
 if(!child){
  struct rlimit core={0,0};if(setrlimit(RLIMIT_CORE,&core)||setpgid(0,0))_exit(120);
  close(output[0]);if(dup2(output[1],1)<0||dup2(output[1],2)<0)_exit(121);close(output[1]);
  if(geteuid()==0&&(setgroups(0,NULL)||setgid(65534)||setuid(65534)))_exit(122);
  char *args[]={argv[1],"subreaper-only",NULL},*env[]={NULL};execve(args[0],args,env);_exit(123);
 }
 close(output[1]);char buffer[4096]={0};size_t used=0;
 while(used<sizeof(buffer)-1){ssize_t n=read(output[0],buffer+used,sizeof(buffer)-1-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;}
 close(output[0]);int status=0;pid_t got;do{got=waitpid(child,&status,0);}while(got<0&&errno==EINTR);
 alarm(0);child=0;
 printf("LINUX_SUBREAPER_RESULT status=%d output=%s",status,buffer);
 if(got<0||!WIFEXITED(status)||WEXITSTATUS(status)||strcmp(buffer,"LINUX_SUBREAPER_PASS\n"))return 1;
 if(getpid()!=parent||getppid()!=ppid||waitpid(-1,&status,WNOHANG)!=-1||errno!=ECHILD)return 1;
 /* A new native child must still use native PID/wait semantics. */
 pid_t control=fork();if(control<0)return 2;if(!control)_exit(29);
 do{got=waitpid(control,&status,0);}while(got<0&&errno==EINTR);
 if(got!=control||!WIFEXITED(status)||WEXITSTATUS(status)!=29)return 1;
 puts("XNU_SUBREAPER_PERSONALITY_PASS: native adoption, live/zombie waits, exec preservation, fork reset; native parent intact");return 0;
}
