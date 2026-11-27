/* Privileged native test admission. Supporting Darwin processes stay outside.
 * This is an isolated diagnostic, not the eventual installer boot launcher. */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <grp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static pid_t child,sentinel;
static void timeout(int signal){(void)signal;if(child>0)kill(-child,SIGKILL);if(sentinel>0)kill(sentinel,SIGKILL);_exit(124);}
static void cleanup(void){if(child>0)kill(-child,SIGKILL);if(sentinel>0)kill(sentinel,SIGKILL);}
static int query(const char *name,uint64_t *value){size_t bytes=sizeof(*value);return sysctlbyname(name,value,&bytes,NULL,0)||bytes!=sizeof(*value);}
static int admit(uint64_t *key){uint32_t command=1;size_t bytes=sizeof(*key);return sysctlbyname("kern.rdxnu_linux_domain",key,&bytes,&command,sizeof(command))||bytes!=sizeof(*key)||!*key;}
static int waitcount(uint64_t expected){for(int i=0;i<100;i++){uint64_t count;if(query("kern.rdxnu_linux_domains_live",&count))return 1;if(count==expected)return 0;usleep(20000);}return 1;}
static int drop(void){return setgroups(0,NULL)||setgid(65534)||setuid(65534);}
static int waitfor(pid_t pid,int code){int status=0;pid_t got;do{got=waitpid(pid,&status,0);}while(got<0&&errno==EINTR);int bad=got!=pid||!WIFEXITED(status)||WEXITSTATUS(status)!=code;if(bad)fprintf(stderr,"NATIVE_DOMAIN_WAIT_FAILURE pid=%d status=%d got=%d\n",pid,status,got);return bad;}

int main(int argc,char **argv){
 if(argc!=3||geteuid()!=0)return 2;
 atexit(cleanup);
 uint64_t baseline,own;
 if(query("kern.rdxnu_linux_domains_live",&baseline)||query("kern.rdxnu_linux_domain",&own)||own)return 2;
 signal(SIGALRM,timeout);alarm(30);pid_t parent=getpid();int channel[2];if(pipe(channel))return 2;
 fflush(NULL);sentinel=fork();if(sentinel<0)return 2;
 if(!sentinel){
  close(channel[0]);uint64_t key;
  if(setsid()<0||admit(&key)||drop()||write(channel[1],&key,sizeof(key))!=sizeof(key))_exit(120);
  close(channel[1]);for(;;)pause();
 }
 close(channel[1]);uint64_t sentinel_key;
 if(read(channel[0],&sentinel_key,sizeof(sentinel_key))!=sizeof(sentinel_key)||!sentinel_key)return 1;
 close(channel[0]);if(waitcount(baseline+1))return 1;
 uint64_t previous=sentinel_key;
 for(int cycle=0;cycle<2;cycle++){
  int output[2],identity[2];if(pipe(output)||pipe(identity))return 2;
  fflush(NULL);child=fork();if(child<0)return 2;
  if(!child){
   struct rlimit core={0,0};uint64_t key=0;
   if(setrlimit(RLIMIT_CORE,&core)||setsid()<0||admit(&key))_exit(121);
   uint64_t ignored;errno=0;
   if(!admit(&ignored)||errno!=EBUSY)_exit(122);
   close(output[0]);close(identity[0]);
   if(write(identity[1],&key,sizeof(key))!=sizeof(key))_exit(123);close(identity[1]);
   if(dup2(output[1],1)<0||dup2(output[1],2)<0)_exit(124);close(output[1]);
   if(drop())_exit(125);
   errno=0;if(!admit(&ignored)||errno!=EPERM)_exit(126);
   char outside[24];snprintf(outside,sizeof(outside),"%d",sentinel);
   char *args[]={argv[1],"member",outside,"native",argv[2],NULL},*env[]={NULL};
   execve(args[0],args,env);_exit(127);
  }
  close(output[1]);close(identity[1]);uint64_t key=0;
  if(read(identity[0],&key,sizeof(key))!=sizeof(key)||!key||key==previous||key==sentinel_key)return 1;
  close(identity[0]);previous=key;
  char buffer[4096]={0};size_t used=0;
  while(used<sizeof(buffer)-1){ssize_t n=read(output[0],buffer+used,sizeof(buffer)-1-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;}
  close(output[0]);if(waitfor(child,0)){fputs(buffer,stderr);return 1;}child=0;
  if(strcmp(buffer,"LINUX_SUBREAPER_PASS\nLINUX_DOMAIN_LIFECYCLE_PASS\n")){fputs(buffer,stderr);return 1;}
  if(waitcount(baseline+1)||kill(sentinel,0)||query("kern.rdxnu_linux_domain",&own)||own||getpid()!=parent)return 1;
  printf("NATIVE_DOMAIN_CYCLE_PASS cycle=%d key=%llu\n",cycle+1,(unsigned long long)key);
 }
 if(kill(sentinel,SIGTERM))return 1;
 int status=0;pid_t got;do{got=waitpid(sentinel,&status,0);}while(got<0&&errno==EINTR);
 if(got!=sentinel||!WIFSIGNALED(status)||WTERMSIG(status)!=SIGTERM)return 1;
 sentinel=0;if(waitcount(baseline))return 1;
 pid_t control=fork();if(control<0)return 2;if(!control)_exit(29);
 if(waitfor(control,29)||waitpid(-1,&status,WNOHANG)!=-1||errno!=ECHILD)return 1;
 alarm(0);puts("XNU_DOMAIN_PERSONALITY_PASS: real domain ownership, scoped targets/adoption, exec continuity, leader-death cleanup and restart; native IDs and parent intact");return 0;
}
