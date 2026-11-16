/* Linux guest regression for virtual pidfd filesystem identity. */
#include <errno.h>
#include <linux/magic.h>
#include <stdio.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
  int fd = (int)syscall(SYS_pidfd_open, getpid(), 0);
  if (fd < 0) return 1;
  struct statfs st = {0};
  if (fstatfs(fd, &st) != 0) return 2;
  /* Shimmy's kqueue-backed pidfd has no pidfs inode identity. */
  if (st.f_type != ANON_INODE_FS_MAGIC) return 3;
  if (fstatfs(fd, NULL) != -1 || errno != EFAULT) return 4;
  if (fstatfs(-1, &st) != -1 || errno != EBADF) return 5;
  close(fd);
  puts("PIDFD_STATFS_PASS");
  return 0;
}
