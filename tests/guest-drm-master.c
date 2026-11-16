// Build as a static AArch64 Linux program and run only in a disposable guest root.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DRM_SET_MASTER _IO('d', 0x1e)
#define DRM_DROP_MASTER _IO('d', 0x1f)

int main(int argc, char **argv) {
  int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (fd < 0) { perror("open card"); return 2; }
  int r = ioctl(fd, DRM_SET_MASTER, 0);
  printf("SET r=%d errno=%d\n", r, r < 0 ? errno : 0);
  fflush(stdout);
  if (argc > 1 && argv[1][0] == 'h') sleep(8);
  if (r == 0) {
    int copy = dup(fd);
    if (copy < 0) return 3;
    close(fd);
    r = ioctl(copy, DRM_SET_MASTER, 0);
    printf("DUP_SET r=%d errno=%d\n", r, r < 0 ? errno : 0);
    r = ioctl(copy, DRM_DROP_MASTER, 0);
    printf("DROP r=%d errno=%d\n", r, r < 0 ? errno : 0);
    close(copy);
  } else close(fd);
  return 0;
}
