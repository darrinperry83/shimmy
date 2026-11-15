#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
int main(void) {
 int fd=open("/ownership-file",O_CREAT|O_RDWR|O_TRUNC,0600); assert(fd>=0);
 assert(syscall(SYS_fchown,fd,974,974)==0);
 struct stat st; assert(fstat(fd,&st)==0&&st.st_uid==974&&st.st_gid==974);
 assert(stat("/ownership-file",&st)==0&&st.st_uid==974&&st.st_gid==974);
 struct statx sx; assert(syscall(SYS_statx,fd,"",AT_EMPTY_PATH,STATX_BASIC_STATS,&sx)==0);
 assert(sx.stx_uid==974&&sx.stx_gid==974);
 assert(syscall(SYS_fchownat,fd,"",-1,977,AT_EMPTY_PATH)==0);
 assert(fstat(fd,&st)==0&&st.st_uid==974&&st.st_gid==977);
 unlink("/ownership-link");assert(symlink("ownership-file","/ownership-link")==0);
 assert(syscall(SYS_fchownat,AT_FDCWD,"/ownership-link",12,34,AT_SYMLINK_NOFOLLOW)==0);
 assert(lstat("/ownership-link",&st)==0&&st.st_uid==12&&st.st_gid==34);
 assert(stat("/ownership-link",&st)==0&&st.st_uid==974&&st.st_gid==977);
 errno=0;assert(syscall(SYS_fchown,-1,1,1)==-1&&errno==EBADF);
 errno=0;assert(syscall(SYS_fchownat,AT_FDCWD,"/not-present",1,1,0)==-1&&errno==ENOENT);
 close(fd);puts("PASS Linux fchown/fchownat, stat/fstat/statx, AT_EMPTY_PATH, symlink and errno semantics");return 0;
}
