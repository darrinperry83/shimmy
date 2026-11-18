#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int expect(int fd, const void *p, size_t len, long long off, int err) {
  errno = 0;
  long n = syscall(SYS_pwrite64, fd, p, len, off);
  return err ? (n == -1 && errno == err) : n == (long)len;
}
int main(void) {
  int fd = open("/work/file", O_RDWR);
  if (fd < 0) return 1;
  if (lseek(fd, 3, SEEK_SET) != 3) return 2;
  if (!expect(fd, (void *)1, 4, 5, EFAULT)) return 3;
  char data[16];
  if (pread(fd, data, sizeof data, 0) != sizeof data ||
      memcmp(data, "abcdefghijklmnop", sizeof data)) return 4;
  if (!expect(fd, "WXYZ", 4, 5, 0)) return 5;
  if (lseek(fd, 0, SEEK_CUR) != 3) return 6;
  if (pread(fd, data, sizeof data, 0) != sizeof data ||
      memcmp(data, "abcdeWXYZjklmnop", sizeof data)) return 7;
  if (!expect(fd, NULL, 0, 5, 0)) return 8;
  if (!expect(fd, "q", 1, -1, EINVAL)) return 9;
  if (!expect(fd, "q", 1024 * 1024 + 1, 0, EOPNOTSUPP)) return 10;
  if (!expect(fd, "q", 2, INT64_MAX, EINVAL)) return 11;
  if (!expect(-1, "q", 1, 0, EBADF)) return 12;
  int ro = open("/work/file", O_RDONLY);
  if (ro < 0 || !expect(ro, "q", 1, 0, EBADF)) return 13;
  int app = open("/work/file", O_WRONLY | O_APPEND);
  if (app < 0 || !expect(app, "q", 1, 0, EOPNOTSUPP)) return 14;
  int pipefd[2];
  if (pipe2(pipefd, 0) || !expect(pipefd[1], "q", 1, 0, ESPIPE)) return 15;
  if (pread(fd, data, sizeof data, 0) != sizeof data ||
      memcmp(data, "abcdeWXYZjklmnop", sizeof data)) return 16;
  close(pipefd[0]); close(pipefd[1]); close(app); close(ro); close(fd);
  puts("GUEST_PWRITE64_CHECKED_OK");
  return 0;
}
