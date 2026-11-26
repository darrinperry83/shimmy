/* Native supervisor stays outside chroot; children get native root/credentials.
 * No PID override, boot marker, userspace syscall broker or library-path env. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <grp.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
static pid_t child;
static void timeout(int sig){(void)sig;if(child>0)kill(-child,SIGKILL);_exit(124);}
static int launch(const char *root,char **args,char **env,int expected){
 fflush(NULL);child=fork();if(child<0)return 1;
 if(!child){
  struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
  if(setpgid(0,0)||chroot(root)||chdir("/")||setgroups(0,NULL)||setgid(65534)||setuid(65534))_exit(120);
  execve(args[0],args+1,env);
  int error=errno;
  if(expected && error==expected)_exit(0);
  dprintf(2,"INTERP_EXEC_ERROR path=%s errno=%d expected=%d\n",args[0],error,expected);_exit(121);
 }
 int status=0;pid_t got;do{got=waitpid(child,&status,0);}while(got<0&&errno==EINTR);
 child=0;return got<0 || !WIFEXITED(status) || WEXITSTATUS(status)!=0;
}
int main(int argc,char **argv){
 if(argc!=2||geteuid()!=0)return 2;
 char root[PATH_MAX],outside[PATH_MAX];if(!realpath(argv[1],root))return 2;
 if(snprintf(outside,sizeof outside,"%s.host-only",root)>=(int)sizeof outside)return 2;
 int fd=open(outside,O_WRONLY|O_CREAT|O_EXCL,0644);if(fd<0)return 2;close(fd);
 pid_t parent=getpid();signal(SIGALRM,timeout);alarm(30);
 char *empty[]={NULL},*environment[]={"RDXNU_INTERP_ENV=preserved",NULL};
 char *probe[]={"/usr/bin/interp-probe","rdxnu-custom-argv0",outside,NULL};
 int failed=launch(root,probe,environment,0);
 char *missing[]={"/usr/bin/missing-probe","missing-probe",outside,NULL};
 char *denied[]={"/usr/bin/denied-probe","denied-probe",outside,NULL};
 char *broken[]={"/usr/bin/broken-probe","broken-probe",outside,NULL};
 failed|=launch(root,missing,empty,ENOENT);
 failed|=launch(root,denied,empty,EACCES);
 failed|=launch(root,broken,empty,EIO);
 if(!failed)puts("INTERP_ERROR_CONTROLS_PASS");
 char *systemd[]={"/usr/lib/systemd/systemd","systemd","--version",NULL};
 failed|=launch(root,systemd,empty,0);
 alarm(0);unlink(outside);
 if(parent!=getpid()||failed)return 1;
 puts("XNU_INTERP_PERSONALITY_PASS: direct Arch exec, auxv/TLS, native root and credentials, interpreter errors, child reap; native parent intact");return 0;
}
