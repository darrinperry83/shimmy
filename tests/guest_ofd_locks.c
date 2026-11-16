// Compile as an aarch64 Linux PIE and run through shim_cli against a disposable
// Arch root. Exercises Linux F_OFD_* numbers and cross-description lifetime,
// not merely the corresponding native macOS fcntl calls.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int check(int ok, const char *what) {
  if (ok) return 0;
  fprintf(stderr, "%s: %s\n", what, strerror(errno));
  return 1;
}

int main(void) {
  const char *path = "/tmp/shimmy-ofd-lock-test";
  int a = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
  int b = open(path, O_RDWR);
  if (check(a >= 0 && b >= 0, "open")) return 1;
  struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET,
                       .l_start = 0, .l_len = 0, .l_pid = 0};
  if (check(fcntl(a, F_OFD_SETLKW, &lock) == 0, "F_OFD_SETLKW")) return 1;
  int duplicate = dup(a);
  if (check(duplicate >= 0, "dup")) return 1;
  close(a);

  struct flock query = lock;
  if (check(fcntl(b, F_OFD_GETLK, &query) == 0 && query.l_type == F_WRLCK,
            "F_OFD_GETLK conflict after dup/close")) return 1;
  errno = 0;
  if (check(fcntl(b, F_OFD_SETLK, &lock) == -1 &&
            (errno == EAGAIN || errno == EACCES), "F_OFD_SETLK contention")) return 1;
  struct flock invalid = lock;
  invalid.l_pid = 123;
  errno = 0;
  if (check(fcntl(b, F_OFD_SETLK, &invalid) == -1 && errno == EINVAL,
            "nonzero OFD l_pid rejected")) return 1;

  close(duplicate);
  if (check(fcntl(b, F_OFD_SETLK, &lock) == 0,
            "OFD lock released on last description close")) return 1;
  lock.l_type = F_UNLCK;
  if (check(fcntl(b, F_OFD_SETLK, &lock) == 0, "F_OFD_SETLK unlock")) return 1;
  close(b);
  unlink(path);
  puts("OFD lock translation: pass");
  return 0;
}
