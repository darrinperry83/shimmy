// Native Darwin regression: exec children must use the renderer's VT, not a private pty.
#include "vt.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
int linux_errno_from_host(int e) { return e; }
int guest_child_known(int pid) { (void)pid; return 0; }
int shim_fd_reserve(int fd) {
  if (fd < 256) { int h=fcntl(fd,F_DUPFD_CLOEXEC,256); if(h>=0){close(fd);fd=h;} }
  fcntl(fd,F_SETFD,FD_CLOEXEC); return fd;
}
int main(int argc, char **argv) {
  char pid[32];
  if (!getenv("RDVT_ROOT_PID")) { snprintf(pid,sizeof pid,"%d",getpid());setenv("RDVT_ROOT_PID",pid,1); }
  if (argc > 1 && !strcmp(argv[1], "stale")) {
    const char *state=getenv("RDVT_STATE"); char bad[1024];
    const char *dev=strchr(state, ',')+1, *ino=strchr(dev, ',')+1, *path=strchr(ino, ',');
    snprintf(bad,sizeof bad,"%.*s0%s",(int)(ino-state),state,path);
    setenv("RDVT_STATE",bad,1);
  }
  vt_init();
  if (argc > 1 && !strcmp(argv[1], "stale")) {
    if(vt_open_node("/dev/tty1")>=0) { fputs("FAIL stale inode accepted\n",stderr);return 7; }
    puts("PASS stale slave identity rejected");return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "reexec")) {
    int fd=vt_open_node("/dev/tty1"); struct stat st;
    if(fd<0 || fstat(fd,&st)) return 5;
    if((unsigned long long)st.st_rdev!=strtoull(argv[2],0,10)) return 6;
    puts("PASS renderer re-exec preserves tty1"); return 0;
  }
  if (argc > 1) {
    // Mimic the executor closing ordinary descriptors before opening StandardInput=tty.
    for (int i=3;i<256;i++) close(i);
    int fd=vt_open_node("/dev/tty1");
    if(fd<0){fprintf(stderr,"FAIL child tty1 open: %d\n",fd);return 1;}
    struct stat st; fstat(fd,&st);
    if((unsigned long long)st.st_rdev != strtoull(argv[1],0,10)){fprintf(stderr,"FAIL child opened different VT\n");return 2;}
    close(fd); fd=vt_open_node("/dev/tty1");
    if(fd<0){fprintf(stderr,"FAIL repeated open %d\n",fd);return 3;}
    puts("PASS child exec/descriptor cleanup/repeated open share renderer tty1");return 0;
  }
  int fd=vt_open_node("/dev/tty1");if(fd<0)return 4;
  struct stat st;fstat(fd,&st);close(fd);
  char dev[32];snprintf(dev,sizeof dev,"%llu",(unsigned long long)st.st_rdev);
  pid_t child=fork();if(child==0){execl(argv[0],argv[0],dev,NULL);_exit(99);}
  int status;waitpid(child,&status,0);
  if(!WIFEXITED(status)||WEXITSTATUS(status))return WIFEXITED(status)?WEXITSTATUS(status):98;
  child=fork();if(child==0){execl(argv[0],argv[0],"stale",NULL);_exit(99);}
  waitpid(child,&status,0);
  if(!WIFEXITED(status)||WEXITSTATUS(status))return WIFEXITED(status)?WEXITSTATUS(status):98;
  execl(argv[0],argv[0],"reexec",dev,NULL);return 99;
}
