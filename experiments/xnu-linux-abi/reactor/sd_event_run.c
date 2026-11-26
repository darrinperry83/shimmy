/* Native parent executes the Arch-linked event loop inside the real root view. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
static pid_t child;
static void timeout(int sig){(void)sig;if(child>0)kill(-child,SIGKILL);_exit(124);}
int main(int argc,char **argv){
 if(argc!=2||geteuid()!=0)return 2;signal(SIGALRM,timeout);alarm(25);pid_t parent=getpid();child=fork();if(child<0)return 2;
 if(!child){struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
  if(setpgid(0,0)||chroot(argv[1])||chdir("/")||setgroups(0,NULL)||setgid(65534)||setuid(65534))_exit(120);
  char *args[]={"/usr/bin/sd-event-probe",NULL},*env[]={NULL};execve(args[0],args,env);perror("sd-event exec");_exit(121);}
 int status=0;while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 2;alarm(0);
 printf("SD_EVENT_NATIVE_RESULT status=%d parent=%d\n",status,getpid());
 if(parent!=getpid()||!WIFEXITED(status)||WEXITSTATUS(status))return 1;
 puts("ARCH_SD_EVENT_NATIVE_PARENT_PASS");return 0;
}
