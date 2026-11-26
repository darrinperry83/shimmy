/* Native parent verifies Linux child VM behavior and preserves Darwin control. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
static pid_t child;
static void timeout(int sig) { (void)sig; if(child>0)kill(child,SIGKILL); _exit(124); }
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    signal(SIGALRM,timeout);alarm(15);
    pid_t parent=getpid();int channel[2];
    if(pipe(channel))return 2;
    int zero=open("/dev/zero",O_RDONLY);
    if(zero<0)return 2;
    child=fork();if(child<0)return 2;
    if(!child){
        struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
        close(channel[0]);
        if(dup2(channel[1],1)<0 || dup2(zero,3)<0)_exit(100);
        if(channel[1]!=1 && channel[1]!=3)close(channel[1]);
        if(zero!=1 && zero!=3)close(zero);
        char *args[]={argv[1],NULL},*env[]={NULL};
        execve(argv[1],args,env);perror("memory ELF exec");_exit(101);
    }
    close(channel[1]);close(zero);
    char buffer[128]={0};size_t used=0;
    while(used<sizeof(buffer)-1){
        ssize_t n=read(channel[0],buffer+used,sizeof(buffer)-1-used);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)break;
        used+=(size_t)n;
    }
    close(channel[0]);int status;
    while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 2;
    alarm(0);
    printf("LINUX_MEMORY_RESULT status=%d native_parent=%d output=%s",status,getpid(),buffer);
    if(!WIFEXITED(status)||WEXITSTATUS(status)||getpid()!=parent||strcmp(buffer,"BALINUX_MEMORY_PASS\n"))return 1;
    puts("XNU_MEMORY_PERSONALITY_PASS: zero fill, RO/NONE/RW, copy faults, partial unmap, holes, errno; native parent intact");
    return 0;
}
