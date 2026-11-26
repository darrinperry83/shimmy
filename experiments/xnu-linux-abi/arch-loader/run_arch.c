/* Native parent executing an unchanged Arch ELF; diagnostics go to the log. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
static volatile sig_atomic_t child_pid;
static void timeout(int sig) {
    (void)sig;
    if (child_pid > 0) kill(child_pid,SIGKILL);
    static const char message[]="ARCH_ELF_TIMEOUT\n";
    (void)write(2,message,sizeof message-1);
    _exit(124);
}
int main(int argc,char **argv) {
    if(argc<2) return 2;
    pid_t parent=getpid();
    signal(SIGALRM,timeout);
    pid_t child=fork();
    if(child<0) return 2;
    if(!child) {
        struct rlimit limit={0,0};
        setrlimit(RLIMIT_CORE,&limit);
        char *env[]={NULL};
        execve(argv[1],argv+1,env);
        perror("ARCH_ELF_EXECVE");
        _exit(101);
    }
    child_pid=child;
    alarm(20);
    int status;
    while(waitpid(child,&status,0)<0) if(errno!=EINTR) return 2;
    alarm(0);
    printf("ARCH_ELF_RESULT child=%d status=%d parent=%d\n",child,status,getpid());
    if(getpid()!=parent || !WIFEXITED(status) || WEXITSTATUS(status)) return 1;
    puts("ARCH_ELF_COMMAND_PASS");
    return 0;
}
