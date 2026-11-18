#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static long raw_sendfile(long out_fd, long in_fd, long offset, long count) {
  register long x0 __asm__("x0") = out_fd;
  register long x1 __asm__("x1") = in_fd;
  register long x2 __asm__("x2") = offset;
  register long x3 __asm__("x3") = count;
  register long x8 __asm__("x8") = 71;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}

int main(void) {
  int in = open("/tmp/sendfile-in", O_CREAT | O_RDWR | O_TRUNC, 0600);
  int out = open("/tmp/sendfile-out", O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (in < 0 || out < 0 || write(in, "abcdef", 6) != 6 || lseek(in, 0, SEEK_SET) != 0) return 1;
  int64_t offset = 2;
  long first = raw_sendfile(out, in, (long)&offset, 3);
  if (first != 3) { fprintf(stderr, "first sendfile=%ld in=%d out=%d offset=%lld\n", first, in, out, (long long)offset); return 2; }
  if (offset != 5 || lseek(in, 0, SEEK_CUR) != 0) return 3;
  if (raw_sendfile(out, in, (long)&offset, 20) != 1 || offset != 6) return 4;
  if (lseek(out, 0, SEEK_SET) != 0) return 5;
  char got[8] = {0};
  if (read(out, got, 4) != 4 || memcmp(got, "cdef", 4)) return 6;
  if (lseek(out, 0, SEEK_END) != 4) return 7;
  if (raw_sendfile(out, in, 0, 2) != 2 || lseek(in, 0, SEEK_CUR) != 2) return 8;
  if (raw_sendfile(out, in, 1, 1) != -14) return 9;
  offset = -1;
  if (raw_sendfile(out, in, (long)&offset, 1) != -22) return 10;
  int append = open("/tmp/sendfile-append", O_CREAT | O_WRONLY | O_APPEND, 0600);
  if (append < 0 || raw_sendfile(append, in, 0, 1) != -22) return 11;
  if (raw_sendfile(in, in, 0, 1) != -22) return 14;
  int readonly = open("/tmp/sendfile-in", O_RDONLY);
  if (readonly < 0 || raw_sendfile(readonly, in, 0, 1) != -9) return 15;
  close(readonly);
  int pipefd[2];
  if (pipe(pipefd) != 0 || raw_sendfile(out, pipefd[0], 0, 1) != -22) return 12;
  close(pipefd[0]); close(pipefd[1]); close(append);
  close(in);
  if (raw_sendfile(out, in, 0, 1) != -9) return 13;
  close(out);
  puts("GUEST_SENDFILE_OK");
  return 0;
}
