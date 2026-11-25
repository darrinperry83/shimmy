/* A ready pipe must wake ppoll even when a second socket is idle. */
static long sc(long nr,long a,long b,long c,long d) {
 register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d,x8 __asm__("x8")=nr;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x8):"memory");return x0;
}
struct pollfd {int fd;short events,revents;};
void _start(void) {
 int pipes[2],socks[2],code=1;
 if(sc(59,(long)pipes,0,0,0)<0)goto done;
 if(sc(199,1,1,0,(long)socks)<0)goto done;
 char byte='x';if(sc(64,pipes[1],(long)&byte,1,0)!=1)goto done;
 struct pollfd fds[2];fds[0].fd=pipes[0];fds[0].events=1;fds[0].revents=0;fds[1].fd=socks[0];fds[1].events=1;fds[1].revents=0;
 if(sc(73,(long)fds,2,0,0)!=1 || fds[0].revents!=1 || fds[1].revents)goto done;
 code=0;{static const char ok[]="PPOLL_PARTIAL_READY_OK\n";sc(64,1,(long)ok,sizeof ok-1,0);}
done:sc(93,code,0,0,0);for(;;){}
}
