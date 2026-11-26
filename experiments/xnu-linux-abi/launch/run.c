/* Native supervisor: real chroot/credentials and two forced child releases. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
static pid_t child;
static void timeout(int sig){(void)sig;if(child>0)kill(-child,SIGKILL);_exit(124);}
static int native_control(void){
 pid_t c=fork();if(c<0)return 1;
 if(!c){if(setsid()!=getpid()||getsid(0)!=getpid()||getpgid(0)!=getpid())_exit(101);
  volatile unsigned char *p=malloc(16*1024*1024);if(!p)_exit(102);
  for(size_t i=0;i<16*1024*1024;i+=4096)p[i]=1;
  _exit(37);}
 int status=0;struct rusage usage={0};if(wait4(c,&status,0,&usage)!=c||!WIFEXITED(status)||WEXITSTATUS(status)!=37)return 1;
#ifdef __APPLE__
 if(usage.ru_maxrss<16*1024*1024)return 1;
#else
 if(usage.ru_maxrss<16*1024)return 1;
#endif
 puts("NATIVE_LAUNCH_CONTROL_PASS");return 0;
}
int main(int argc,char **argv){
 if(argc!=2||geteuid()!=0)return 2;signal(SIGALRM,timeout);alarm(40);if(native_control())return 1;
 char work[PATH_MAX];if(snprintf(work,sizeof work,"%s/work/run.XXXXXX",argv[1])>=(int)sizeof work)return 2;
 if(!mkdtemp(work)||chown(work,65534,65534))return 2;
 const char *inside=work+strlen(argv[1]);if(strncmp(inside,"/work/",6))return 2;
 int input[2],output[2];if(pipe(input)||pipe(output))return 2;fflush(stdout);
 pid_t parent=getpid();child=fork();if(child<0)return 2;
 if(!child){struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);if(setpgid(0,0))_exit(120);
  close(input[1]);close(output[0]);if(dup2(output[1],1)<0||dup2(input[0],3)<0)_exit(120);
  if(input[0]>3)close(input[0]);if(output[1]>3)close(output[1]);
  if(chroot(argv[1])||chdir(inside)||setgroups(0,NULL)||setgid(65534)||setuid(65534))_exit(120);
  char *args[]={"/usr/bin/launch-probe",NULL},*env[]={NULL};execve(args[0],args,env);perror("launch exec");_exit(121);}
 close(input[0]);close(output[1]);char buffer[8192]={0};size_t used=0;int released=0;
 const char *waits[]={"LAUNCH_GROUP_WAIT\n","LAUNCH_EXEC_WAIT\n"},*ready[]={"LAUNCH_GROUP_CHILD_READY\n","LAUNCH_EXEC_CHILD_READY\n"};
 while(used<sizeof buffer-1){ssize_t n=read(output[0],buffer+used,sizeof buffer-1-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=n;
  if(released<2&&strstr(buffer,waits[released])&&strstr(buffer,ready[released])){usleep(200000);int status=0;
   if(waitpid(child,&status,WNOHANG)!=0||write(input[1],"T",1)!=1)return 1;released++;}}
 close(input[1]);close(output[0]);int status=0;while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 2;
 printf("LINUX_LAUNCH_RESULT status=%d native_parent=%d releases=%d output=%s",status,getpid(),released,buffer);
 if(parent!=getpid()||!WIFEXITED(status)||WEXITSTATUS(status)||released!=2||!strstr(buffer,"LINUX_LAUNCH_PASS\n")||!strstr(buffer,"ARCH_LAUNCH_CHILD_PASS\n"))return 1;
 alarm(0);puts("XNU_LAUNCH_PERSONALITY_PASS: native pipes, descriptor remap, sessions/groups, limits, wait4/rusage and direct Arch child exec; native parent intact");return 0;
}
