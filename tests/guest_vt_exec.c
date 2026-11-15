#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
int main(void) {
 DIR *d=opendir("/proc/self/fd"); if(!d){perror("opendir");return 1;}
 struct dirent *e;
 while((e=readdir(d))){char *end;long fd=strtol(e->d_name,&end,10);if(*end||fd<3||fd==dirfd(d))continue;
  if(fd>=256){fprintf(stderr,"FAIL private fd exposed: %ld\n",fd);return 2;}
  if(fcntl(fd,F_SETFD,FD_CLOEXEC)<0&&errno!=EBADF){perror("fcntl");return 3;}
 }
 closedir(d);puts("PASS guest fd sweep: no private descriptors");fflush(stdout);
 execl("/usr/bin/agetty","agetty","--noclear","tty1","38400","linux",NULL);perror("exec");return 4;
}
