#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

_Static_assert(sizeof(struct stat) == 128, "aarch64 Linux stat ABI changed");

int main(void) {
  struct { struct stat value; uint64_t canary; } out = {
      .canary = UINT64_C(0x7a05d18ec3b42f69) };
  if (syscall(SYS_newfstatat, AT_FDCWD, "/tmp/stat79-fixture", &out.value, 0) != 0 ||
      !S_ISREG(out.value.st_mode) || out.value.st_size != 7 ||
      out.canary != UINT64_C(0x7a05d18ec3b42f69)) return 1;
  int fd = open("/tmp/stat79-fixture", O_RDONLY);
  if (fd < 0) return 2;
  if (syscall(SYS_newfstatat, fd, "", &out.value, AT_EMPTY_PATH) != 0 ||
      !S_ISREG(out.value.st_mode) || out.value.st_size != 7) return 3;
  if (syscall(SYS_newfstatat, fd, NULL, &out.value, AT_EMPTY_PATH) != 0 ||
      out.value.st_size != 7) return 4;
  errno = 0;
  if (syscall(SYS_newfstatat, AT_FDCWD, "/tmp/stat79-fixture", NULL, 0) != -1 ||
      errno != EFAULT) return 5;
  errno = 0;
  if (syscall(SYS_newfstatat, AT_FDCWD, "/tmp/stat79-fixture", (void *)1, 0) != -1 ||
      errno != EFAULT) return 6;
  errno = 0;
  if (syscall(SYS_newfstatat, fd, "", (void *)1, AT_EMPTY_PATH) != -1 ||
      errno != EFAULT) return 7;
  if (syscall(SYS_newfstatat, AT_FDCWD, "/proc/self/mounts", &out.value, 0) != 0 ||
      !S_ISREG(out.value.st_mode)) return 8;
  if (syscall(SYS_newfstatat, AT_FDCWD, "/dev/tty1", &out.value, 0) != 0 ||
      !S_ISCHR(out.value.st_mode)) return 9;
  dev_t tty_dev = out.value.st_dev;
  ino_t tty_ino = out.value.st_ino;
  int tty = open("/dev/tty1", O_RDONLY | O_NONBLOCK);
  if (tty < 0) return 10;
  if (syscall(SYS_newfstatat, tty, "", &out.value, AT_EMPTY_PATH) != 0 ||
      !S_ISCHR(out.value.st_mode) || out.value.st_dev != tty_dev ||
      out.value.st_ino != tty_ino) return 11;
  close(tty);
  if (out.canary != UINT64_C(0x7a05d18ec3b42f69)) return 12;
  close(fd);
  puts("NEWFSTATAT_CHECKED_PASS");
  return 0;
}
