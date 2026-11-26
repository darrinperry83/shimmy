/* Native Darwin host for real Linux-core operations, not a Linux process host.
 * Does not run a VM, patch guest instructions, or install a kernel.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/utsname.h>
#include <lkl.h>
#include <lkl_host.h>
#include <lkl/linux/eventpoll.h>
#include <lkl/linux/fcntl.h>
#include <lkl/linux/errno.h>

static void timeout(int sig)
{
    (void)sig;
    static const char message[]="LINUX_CORE_TIMEOUT\n";
    (void)write(2,message,sizeof(message)-1);
    _exit(124);
}
static void require(int condition,const char *operation,long result)
{
    if(!condition) {
        fprintf(stderr,"LINUX_CORE_FAIL operation=%s result=%ld\n",operation,result);
        exit(1);
    }
}
int main(void)
{
    setbuf(stdout,NULL);
    pid_t native_pid=getpid();
    struct utsname before,after;
    require(uname(&before)==0,"native uname",-1);
    require(!strcmp(before.sysname,"Darwin"),"native Darwin host",-1);
    signal(SIGALRM,timeout);alarm(30);
    long ret=lkl_init(&lkl_host_ops);
    require(ret==0,"lkl_init",ret);
    ret=lkl_start_kernel("");
    require(ret==0,"lkl_start_kernel",ret);
    puts("LINUX_CORE_BOOT_PASS");

    /* These descriptors and readiness queues are owned by Linux, not Darwin. */
    int fds[2]={-1,-1};
    ret=lkl_sys_pipe2(fds,LKL_O_NONBLOCK);
    require(ret==0,"Linux pipe2",ret);
    char buffer[64]={0};
    ret=lkl_sys_read(fds[0],buffer,sizeof(buffer));
    require(ret==-LKL_EAGAIN,"empty nonblocking Linux read",ret);
    int ep=(int)lkl_sys_epoll_create1(0);
    require(ep>=0,"Linux epoll_create1",ep);
    struct lkl_epoll_event event={.events=LKL_EPOLLIN,.data=0x5244584e55434f52ULL};
    ret=lkl_sys_epoll_ctl(ep,LKL_EPOLL_CTL_ADD,fds[0],&event);
    require(ret==0,"Linux epoll_ctl",ret);
    struct lkl_epoll_event ready={0};
    ret=lkl_sys_epoll_wait(ep,&ready,1,0);
    require(ret==0,"Linux empty epoll",ret);
    const char payload[]="Linux owns this pipe and epoll";
    ret=lkl_sys_write(fds[1],payload,sizeof(payload));
    require(ret==(long)sizeof(payload),"Linux pipe write",ret);
    ret=lkl_sys_epoll_wait(ep,&ready,1,1000);
    require(ret==1 && ready.data==event.data && (ready.events&LKL_EPOLLIN),"Linux epoll readiness",ret);
    ret=lkl_sys_read(fds[0],buffer,sizeof(buffer));
    require(ret==(long)sizeof(payload) && !memcmp(buffer,payload,sizeof(payload)),"Linux pipe payload",ret);
    ret=lkl_sys_epoll_wait(ep,&ready,1,0);
    require(ret==0,"Linux epoll drained",ret);
    require(lkl_sys_close(fds[1])==0,"Linux close writer",-1);
    ret=lkl_sys_read(fds[0],buffer,sizeof(buffer));
    require(ret==0,"Linux pipe EOF",ret);
    require(lkl_sys_close(fds[0])==0 && lkl_sys_close(ep)==0,"Linux descriptor cleanup",-1);
    ret=lkl_sys_read(-1,buffer,1);
    require(ret==-LKL_EBADF,"Linux EBADF",ret);
    puts("LINUX_CORE_PIPE_EPOLL_PASS");

    ret=lkl_sys_mkdir("/core-smoke",0700);
    require(ret==0,"Linux VFS mkdir",ret);
    int fd=(int)lkl_sys_open("/core-smoke/data",LKL_O_CREAT|LKL_O_EXCL|LKL_O_RDWR,0600);
    require(fd>=0,"Linux VFS create",fd);
    ret=lkl_sys_write(fd,payload,sizeof(payload));
    require(ret==(long)sizeof(payload),"Linux VFS write",ret);
    ret=lkl_sys_lseek(fd,0,0);
    require(ret==0,"Linux VFS seek",ret);
    memset(buffer,0,sizeof(buffer));
    ret=lkl_sys_read(fd,buffer,sizeof(buffer));
    require(ret==(long)sizeof(payload) && !memcmp(buffer,payload,sizeof(payload)),"Linux VFS read",ret);
    require(lkl_sys_close(fd)==0 && lkl_sys_unlink("/core-smoke/data")==0 &&
            lkl_sys_rmdir("/core-smoke")==0,"Linux VFS cleanup",-1);
    puts("LINUX_CORE_VFS_PASS");
    ret=lkl_sys_halt();
    require(ret==0,"Linux halt",ret);
    lkl_cleanup();
    alarm(0);
    require(getpid()==native_pid && uname(&after)==0 && !strcmp(after.sysname,before.sysname),"native Darwin host preserved",-1);
    puts("NATIVE_DARWIN_CONTROL_PASS");
    puts("LINUX_CORE_SMOKE_PASS: native Mach-O library; no VM; no Arch ELF or systemd claim");
    return 0;
}
