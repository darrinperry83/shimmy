/* Reproduce the GnuPG 2.4.9 common/dotlock.c hardlink-based lock take, in the
 * guest, against the shim. Mirrors dotlock_create_unix + dotlock_take_unix
 * ("standard method") exactly: a stale lock held by a dead pid must be
 * detected, unlinked, and re-taken in at most a couple of iterations. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *dir = "/tmp/dotlock-probe";
static char stale_tname[128], tname[128], lockname[128];

static void report(int step, int fail, const char *what, long a, long b) {
  printf("STEP %d %s what=%s a=%ld b=%ld errno=%d\n",
         step, fail ? "FAIL" : "ok", what, a, b, errno);
}

static int write_lockfile(const char *path, int pid) {
  char buf[80];
  int n = snprintf(buf, sizeof buf, "%10d\n%s\n", pid, "localhost");
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) return -1;
  ssize_t w = write(fd, buf, (size_t)n);
  int e = errno;
  close(fd);
  return (w == n) ? 0 : -1;
}

static int read_lockfile(const char *path, int *pid_out, int *same_node) {
  char buf[80];
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n < 11) return -1;
  buf[n] = 0;
  *pid_out = atoi(buf);
  /* expected_len = 10 + 1 + len("localhost") + 1 */
  *same_node = (n == (10 + 1 + 9 + 1)) && (strncmp(buf + 11, "localhost", 9) == 0);
  return 0;
}

int main(void) {
  mkdir(dir, 0755);
  snprintf(stale_tname, sizeof stale_tname, "%s/.#lk0xdeadbeef.localhost.99999", dir);
  snprintf(lockname, sizeof lockname, "%s/sentinel.lock", dir);
  snprintf(tname, sizeof tname, "%s/.#lk0xcafebabe.localhost.%d", dir, (int)getpid());

  /* stale lock: content is pid 99999 + localhost, and lockname is a hard link
   * to stale_tname so it has st_nlink == 2. */
  if (write_lockfile(stale_tname, 99999) != 0) { report(0, 1, "write stale", 0, 0); return 1; }
  if (link(stale_tname, lockname) != 0) { report(0, 1, "link stale", 0, 0); return 2; }
  if (write_lockfile(tname, (int)getpid()) != 0) { report(0, 1, "write tname", 0, 0); return 3; }

  /* dotlock_take_unix "standard method" loop. */
  int acquired = 0;
  for (int iter = 0; iter < 8 && !acquired; iter++) {
    /* (void) link(h->tname, h->lockname); */
    errno = 0;
    long lr = link(tname, lockname);
    int link_errno = errno;

    struct stat sb;
    if (stat(tname, &sb) != 0) { report(1, 1, "stat tname", lr, link_errno); return 4; }
    printf("iter=%d link_rc=%ld link_errno=%d stat_nlink=%lu\n",
           iter, lr, link_errno, (unsigned long)sb.st_nlink);

    if (sb.st_nlink == 2) {
      acquired = 1;
      printf("ACQUIRED at iter=%d\n", iter);
      break;
    }

    int pid = -1, same = 0;
    if (read_lockfile(lockname, &pid, &same) != 0) {
      report(2, 1, "read lockname", lr, link_errno);
      return 5;
    }
    printf("iter=%d lockpid=%d same_node=%d\n", iter, pid, same);

    int kr = kill(pid, 0);
    int kerr = errno;
    printf("iter=%d kill(%d,0)=%d errno=%d\n", iter, pid, kr, kerr);
    if (same && kr != 0 && kerr == ESRCH) {
      errno = 0;
      long ur = unlink(lockname);
      int uerr = errno;
      printf("iter=%d unlink_rc=%ld unlink_errno=%d (stale %d)\n", iter, ur, uerr, pid);
    } else {
      printf("iter=%d lock held/not-stale; giving up\n", iter);
      return 6;
    }
  }

  if (!acquired) {
    printf("NOT ACQUIRED after loop\n");
    return 7;
  }
  printf("GUEST_DOTLOCK_OK\n");
  return 0;
}
