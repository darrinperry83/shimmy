#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static long raw_sync_file_range(long fd, long offset, long nbytes, long flags) {
  register long x0 __asm__("x0") = fd;
  register long x1 __asm__("x1") = offset;
  register long x2 __asm__("x2") = nbytes;
  register long x3 __asm__("x3") = flags;
  register long x8 __asm__("x8") = 84;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}

int main(void) {
  int fd = open("/tmp/sync-file-range-probe", O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (fd < 0 || write(fd, "x", 1) != 1) return 1;
  if (raw_sync_file_range(fd, 0, 0, 0) != 0) return 2;
  if (raw_sync_file_range(fd, 1, 1, 0) != 0) return 3;
  if (raw_sync_file_range(fd, -1, 1, 0) != -22) return 4;
  if (raw_sync_file_range(fd, 0, -1, 0) != -22) return 5;
  if (raw_sync_file_range(fd, INT64_MAX, 1, 0) != -22) return 6;
  if (raw_sync_file_range(fd, 0, 1, 8) != -22) return 7;
  if (raw_sync_file_range(fd, 0, 1, 2) != -95) return 8;
  int dir = open("/tmp", O_RDONLY | O_DIRECTORY);
  if (dir < 0 || raw_sync_file_range(dir, 0, 0, 0) != 0) return 11;
  close(dir);
  int pipefd[2];
  if (pipe(pipefd) != 0 || raw_sync_file_range(pipefd[0], 0, 0, 0) != -29) return 9;
  close(pipefd[0]); close(pipefd[1]);
  close(fd);
  if (raw_sync_file_range(fd, 0, 0, 0) != -9) return 10;
  puts("GUEST_SYNC_FILE_RANGE_OK");
  return 0;
}
