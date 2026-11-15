#include "guest_ownership.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
static void check(const struct stat *st,unsigned u,unsigned g) {
  uint32_t a,b; assert(guest_ownership_get(st,&a,&b)==0);assert(a==u&&b==g);
}
int main(int argc,char **argv) {
  struct stat st,after;
  if(argc==3) {guest_ownership_set_root(argv[1]);assert(stat(argv[2],&st)==0);check(&st,974,977);return 0;}
  char root[]="/tmp/rd-ownership-test-XXXXXX";assert(mkdtemp(root));
  guest_ownership_set_root(root);
  char path[1024],link[1024];snprintf(path,sizeof path,"%s/file",root);
  int fd=open(path,O_RDWR|O_CREAT|O_EXCL,0600);assert(fd>=0);assert(fstat(fd,&st)==0);
  check(&st,0,0);assert(guest_ownership_set(&st,974,974)==0);check(&st,974,974);
  assert(guest_ownership_set(&st,UINT32_MAX,977)==0);check(&st,974,977);
  assert(guest_ownership_set(&st,UINT32_MAX,UINT32_MAX)==0);check(&st,974,977);
  snprintf(link,sizeof link,"%s/link",root);assert(symlink("file",link)==0);
  struct stat ls;assert(lstat(link,&ls)==0);assert(guest_ownership_set(&ls,12,34)==0);check(&ls,12,34);
  assert(stat(link,&after)==0);check(&after,974,977);
  assert(fstat(fd,&after)==0);assert(after.st_uid==st.st_uid&&after.st_gid==st.st_gid&&after.st_mode==st.st_mode);
  pid_t p=fork();assert(p>=0);if(!p){execl(argv[0],argv[0],root,path,NULL);_exit(99);}
  int status;assert(waitpid(p,&status,0)==p);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
  char renamed[1024];snprintf(renamed,sizeof renamed,"%s/renamed",root);assert(rename(path,renamed)==0);
  assert(stat(renamed,&after)==0);check(&after,974,977);
  struct stat reused=st;reused.st_birthtimespec.tv_nsec++;check(&reused,0,0);
  pid_t a=fork();assert(a>=0);if(!a){for(int i=0;i<25;i++)assert(guest_ownership_set(&st,42,UINT32_MAX)==0);_exit(0);}
  pid_t b=fork();assert(b>=0);if(!b){for(int i=0;i<25;i++)assert(guest_ownership_set(&st,UINT32_MAX,43)==0);_exit(0);}
  assert(waitpid(a,&status,0)==a&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  assert(waitpid(b,&status,0)==b&&WIFEXITED(status)&&WEXITSTATUS(status)==0);check(&st,42,43);
  char record[1400];snprintf(record,sizeof record,"%s/.rdshim-ownership/%llx-%llx-%llx-%lx",root,
    (unsigned long long)st.st_dev,(unsigned long long)st.st_ino,
    (unsigned long long)st.st_birthtimespec.tv_sec,(unsigned long)st.st_birthtimespec.tv_nsec);
  int rf=open(record,O_WRONLY|O_TRUNC);assert(rf>=0);close(rf);check(&st,0,0);
  rf=open(record,O_WRONLY);assert(rf>=0);assert(write(rf,"x",1)==1);close(rf);
  uint32_t u,g;errno=0;assert(guest_ownership_get(&st,&u,&g)==-1&&errno==EIO);
  errno=0;assert(guest_ownership_set(&st,1,2)==-1&&errno==EIO);
  close(fd);printf("PASS ownership fallback, chown, -1, symlink isolation, unchanged host ownership/mode, exec visibility, rename, inode-generation isolation concurrent partial updates, empty initial record and corruption errors (%s)\n",root);
}
