#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int call(int fd, int mode, long long off, long long len, int error) {
  errno = 0;
  long rc = syscall(SYS_fallocate, fd, mode, off, len);
  return error ? (rc == -1 && errno == error) : rc == 0;
}
int main(void) {
  int fd = open("/work/file", O_RDWR);
  if (fd < 0) return 1;
  if (lseek(fd, 5, SEEK_SET) != 5) return 2;
  if (!call(fd, FALLOC_FL_ZERO_RANGE, 8, 16, 0)) return 3;
  if (lseek(fd, 0, SEEK_CUR) != 5) return 4;
  struct stat st;
  if (fstat(fd, &st) || st.st_size != 32) return 5;
  char data[32];
  if (pread(fd, data, sizeof data, 0) != sizeof data) return 6;
  for (int i = 0; i < 32; i++) {
    char expected = i >= 8 && i < 24 ? 0 : (char)('A' + i);
    if (data[i] != expected) return 7;
  }
  if (!call(fd, FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE, 0, 2, 0)) return 8;
  if (fstat(fd, &st) || st.st_size != 32) return 9;
  if (!call(fd, FALLOC_FL_ZERO_RANGE, 30, 4, EOPNOTSUPP)) return 10;
  if (!call(fd, FALLOC_FL_ZERO_RANGE | FALLOC_FL_PUNCH_HOLE, 2, 2, EOPNOTSUPP)) return 11;
  if (!call(fd, FALLOC_FL_ZERO_RANGE, INT64_MAX - 1, 4, EOVERFLOW)) return 12;
  if (pread(fd, data, sizeof data, 0) != sizeof data) return 13;
  if (data[0] || data[1] || data[2] != 'C' || data[24] != 'Y') return 14;
  int ro = open("/work/file", O_RDONLY);
  if (ro < 0 || !call(ro, FALLOC_FL_ZERO_RANGE, 2, 2, EBADF)) return 15;
  if (!call(-1, FALLOC_FL_ZERO_RANGE, 2, 2, EBADF)) return 16;
  int app = open("/work/file", O_WRONLY | O_APPEND);
  if (app < 0 || !call(app, FALLOC_FL_ZERO_RANGE, 2, 2, EOPNOTSUPP)) return 17;
  close(app); close(ro); close(fd);
  puts("GUEST_FALLOCATE_ZERO_RANGE_OK");
  return 0;
}
