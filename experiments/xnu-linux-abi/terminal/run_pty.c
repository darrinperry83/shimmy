/* Same native PTY/ELF harness runs on macOS/XNU and real AArch64 Linux. */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static volatile sig_atomic_t child_pid;
static char transcript[8192];
static size_t used;
static void timeout(int sig)
{
    (void)sig;
    if(child_pid>0) kill(child_pid,SIGKILL);
    const char message[]="LINUX_PTY_TIMEOUT\n";
    (void)write(2,message,sizeof(message)-1);
    _exit(124);
}
static int expect(int master,const char *marker)
{
    while(!strstr(transcript,marker)) {
        if(used==sizeof(transcript)-1) return -1;
        ssize_t n=read(master,transcript+used,sizeof(transcript)-used-1);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) return -1;
        used+=(size_t)n;transcript[used]=0;
    }
    return 0;
}
int main(int argc,char **argv)
{
    if(argc!=2) return 2;
    int master,slave,aux[2];
    if(openpty(&master,&slave,NULL,NULL,NULL) || pipe(aux)) return 2;
    /* Fix the line discipline so backspace/EOF assertions are deterministic. */
    struct termios t;
    if(tcgetattr(slave,&t)) return 2;
    t.c_lflag |= ICANON|ECHO|ECHOE;
    t.c_lflag &= ~(ECHONL);
    t.c_iflag &= ~(IGNCR|INLCR);
    t.c_iflag |= ICRNL;
    t.c_cc[VERASE]=127;t.c_cc[VEOF]=4;
    if(tcsetattr(slave,TCSANOW,&t) || write(aux[1],"X",1)!=1) return 2;
    close(aux[1]);
    int faultfd=fcntl(aux[0],F_DUPFD,10);
    if(faultfd<0) return 2;
    close(aux[0]);
    pid_t parent=getpid();
    signal(SIGALRM,timeout);alarm(15);
    pid_t child=fork();if(child<0) return 2;
    if(!child) {
        close(master);
        if(setsid()<0 || ioctl(slave,TIOCSCTTY,0)<0) _exit(101);
        if(dup2(slave,0)<0 || dup2(slave,1)<0 || dup2(slave,2)<0) _exit(102);
        if(slave>2) close(slave);
        if(dup2(faultfd,3)<0) _exit(103);
        close(faultfd);
        char *env[]={NULL};char *args[]={argv[1],NULL};
        execve(argv[1],args,env);_exit(104);
    }
    child_pid=child;close(slave);close(faultfd);
    int ok=expect(master,"READ_READY")==0;
    if(ok) ok=write(master,"abc\177D\n",6)==6 && expect(master,"READ_LINE_PASS")==0;
    if(ok) ok=write(master,"\004",1)==1 && expect(master,"READ_EOF_PASS")==0;
    if(!ok) kill(child,SIGKILL);
    int status=0;
    while(waitpid(child,&status,0)<0) if(errno!=EINTR) return 2;
    alarm(0);close(master);
    printf("%s",transcript);
    printf("LINUX_PTY_RESULT child=%d status=%d parent=%d\n",child,status,getpid());
    if(!ok || parent!=getpid() || !WIFEXITED(status) || WEXITSTATUS(status)) return 1;
    puts("LINUX_PTY_READ_PASS: EBADF, zero length, EFAULT, canonical erase, EOF; native parent intact");
    return 0;
}
