/* Native parent forces a blocking read, sends real SIGUSR1 and checks Linux exit. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static pid_t child;
static void timeout(int sig){(void)sig;if(child>0)kill(child,SIGKILL);_exit(124);}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    signal(SIGALRM,timeout);alarm(20);
    int input[2],output[2];if(pipe(input)||pipe(output))return 2;
    pid_t parent=getpid();child=fork();if(child<0)return 2;
    if(!child){
        struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
        close(input[1]);close(output[0]);
        if(dup2(output[1],1)<0)_exit(100);
        close(output[1]);
        if(dup2(input[0],3)<0)_exit(100);
        if(input[0]>3)close(input[0]);
        char *args[]={argv[1],NULL},*env[]={NULL};
        execve(argv[1],args,env);perror("startup exec");_exit(101);
    }
    close(input[0]);close(output[1]);
    const char expected[]="STARTUP_READY\nLINUX_STARTUP_PASS\n";
    char buffer[256]={0};size_t used=0;int released=0;
    while(used<sizeof(buffer)-1){
        ssize_t n=read(output[0],buffer+used,sizeof(buffer)-1-used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)break;
        used+=(size_t)n;
        if(!released && used>=sizeof("STARTUP_READY\n")-1){
            if(memcmp(buffer,"STARTUP_READY\n",sizeof("STARTUP_READY\n")-1))return 1;
            usleep(200000);
            if(kill(child,SIGUSR1)||write(input[1],"T",1)!=1)return 1;
            released=1;close(input[1]);
        }
    }
    if(!released)close(input[1]);
    close(output[0]);int status=0;
    while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 2;
    alarm(0);
    printf("LINUX_STARTUP_RESULT status=%d native_parent=%d output=%s",status,getpid(),buffer);
    if(getpid()!=parent||!WIFEXITED(status)||WEXITSTATUS(status)||strcmp(buffer,expected))return 1;
    puts("XNU_STARTUP_PERSONALITY_PASS: heap growth/shrink/collision, TLS/x18 through blocking read, signal ignore, native clocks, libc support; native parent intact");
    return 0;
}
