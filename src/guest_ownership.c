#include "guest_ownership.h"
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/file.h>

/* Root-local records survive exec/reboot, and are shared by all shim processes.
 * Birth time prevents stale ownership from following a reused inode. No open
 * descriptors survive calls, so executor close-range and fd packing are safe.
 * This is compatibility metadata, NOT a guest security boundary. */
static char metadata[PATH_MAX];
void guest_ownership_set_root(const char *root) {
  if (!root || !*root || !strcmp(root,"/")) { metadata[0]=0; return; }
  // An installed rootfs may keep its top directory read-only while /var/lib
  // remains writable. Preserve older records when present, but put new state
  // under /var/lib so virtual chown works without changing the root mode.
  char legacy[PATH_MAX];
  int ln=snprintf(legacy,sizeof legacy,"%s/.rdshim-ownership",root);
  struct stat st;
  int legacy_exists=0;
  if (ln>0 && (size_t)ln<sizeof legacy) {
    int result=lstat(legacy,&st);
    // Only ENOENT means it is safe to select new storage. An inaccessible or
    // malformed legacy path must not silently hide the old ownership records.
    legacy_exists=result==0 || errno!=ENOENT;
  }
  int n=snprintf(metadata,sizeof metadata,legacy_exists ?
                 "%s/.rdshim-ownership" : "%s/var/lib/.rdshim-ownership",root);
  if(n<0 || (size_t)n>=sizeof metadata) metadata[0]=0;
}
static int open_record(const struct stat *st, int create) {
  if(!metadata[0]) { errno=ENOTSUP; return -1; }
  if(create && mkdir(metadata,0700)<0 && errno!=EEXIST) return -1;
  int d=open(metadata,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
  if(d<0) return -1;
  struct stat ds;
  if(fstat(d,&ds)<0 || ds.st_uid!=getuid() || (ds.st_mode & 0022)) {
    close(d); errno=EACCES; return -1;
  }
  char key[160];
  snprintf(key,sizeof key,"%llx-%llx-%llx-%lx",(unsigned long long)st->st_dev,
    (unsigned long long)st->st_ino,(unsigned long long)st->st_birthtimespec.tv_sec,
    (unsigned long)st->st_birthtimespec.tv_nsec);
  int fd=openat(d,key,(create?O_RDWR|O_CREAT:O_RDONLY)|O_NOFOLLOW|O_CLOEXEC,0600);
  int e=errno; close(d); errno=e;
  if(fd>=0) {
    struct stat fs;
    if(fstat(fd,&fs)<0 || !S_ISREG(fs.st_mode) || fs.st_uid!=getuid() || fs.st_nlink!=1) {
      close(fd); errno=EACCES; return -1;
    }
  }
  return fd;
}
int guest_ownership_get(const struct stat *st,uint32_t *uid,uint32_t *gid) {
  *uid=0; *gid=0;
  int fd=open_record(st,0);
  if(fd<0) return errno==ENOENT || errno==ENOTSUP ? 0 : -1;
  if(flock(fd,LOCK_SH)<0) { int e=errno;close(fd);errno=e;return -1; }
  uint32_t v[2]; ssize_t n=pread(fd,v,sizeof v,0);
  int e=errno; close(fd);
  /* Empty publication before the creator takes LOCK_EX is the pre-chown
   * root default. Nonempty partial records are corruption, never root. */
  if(n==0) return 0;
  if(n!=sizeof v) { errno=n<0?e:EIO; return -1; }
  *uid=v[0]; *gid=v[1]; return 0;
}
int guest_ownership_set(const struct stat *st,uint32_t uid,uint32_t gid) {
  int fd=open_record(st,1); if(fd<0)return -1;
  if(flock(fd,LOCK_EX)<0) { int e=errno;close(fd);errno=e;return -1; }
  uint32_t v[2]={0,0}; ssize_t n=pread(fd,v,sizeof v,0);
  if(n!=0 && n!=sizeof v) { int e=n<0?errno:EIO;close(fd);errno=e;return -1; }
  if(uid!=UINT32_MAX)v[0]=uid;
  if(gid!=UINT32_MAX)v[1]=gid;
  int r=0;
  if(pwrite(fd,v,sizeof v,0)!=sizeof v || fsync(fd)<0)r=-1;
  int e=errno; close(fd);errno=e;return r;
}
