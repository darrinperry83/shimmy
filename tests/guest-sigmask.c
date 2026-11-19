/* Signal-mask probe for the shim's pselect6 (72) / ppoll (73) path.
 *
 * Verifies that a NON-NULL signal mask is no longer rejected with EOPNOTSUPP:
 *   * pselect6 with a mask and a short timeout returns 0 (timeout), and the
 *     mask is restored afterward;
 *   * ppoll with a mask and a short timeout returns 0 (timeout);
 *   * a parent-death signal that becomes pending while masked makes pselect6
 *     return -EINTR (with the mask restored).
 *
 * Emits SIGMASK_GUEST_OK and SIGMASK_EINTR_OK on success.
 *
 * The interrupt path uses a THREE-process tree. The shim's boot_root_watch
 * tears down every forked service when PID 1 (this probe, when run as init)
 * exits, so the dying parent cannot be init. Instead init stays alive, a
 * middle process dies, and its child (the leaf) sees the parent-death signal.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static long raw_syscall6(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}

static long sys_pselect6(int nfds, void* r, void* w, void* e,
                         const struct timespec* ts, void* sigarg) {
  return raw_syscall6(72, nfds, (long)r, (long)w, (long)e, (long)ts, (long)sigarg);
}
static long sys_ppoll(struct pollfd* fds, unsigned long nfds,
                      const struct timespec* ts, const void* mask, size_t sz) {
  return raw_syscall6(73, (long)fds, (long)nfds, (long)ts, (long)mask, (long)sz, 0);
}
static long sys_prctl(int option, unsigned long arg2) {
  return raw_syscall6(167, option, (long)arg2, 0, 0, 0, 0);
}
static long sys_rt_sigprocmask(int how, const void* set, void* old) {
  return raw_syscall6(135, how, (long)set, (long)old, 8, 0, 0);
}

static void write_result(const char* s) {
  int f = open("/tmp/sigmask_result", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (f >= 0) {
    write(f, s, strlen(s));
    close(f);
  }
}

static int test_pselect6_timeout(void) {
  uint64_t mask = 1ULL << (SIGUSR1 - 1);
  struct { uint64_t ss; uint64_t len; } sigarg = { (uint64_t)&mask, sizeof mask };
  struct timespec ts = { 0, 100000000 };  // 100ms
  long n = sys_pselect6(0, NULL, NULL, NULL, &ts, &sigarg);
  if (n != 0) {
    printf("sigmask: pselect6 timeout n=%ld errno=%d\n", n, errno);
    return 1;
  }
  // The wait mask must be restored, not left installed.
  uint64_t old = 0xdeadbeefdeadbeefULL;
  if (sys_rt_sigprocmask(2 /*SIG_SETMASK*/, NULL, &old) != 0 || old != 0) {
    printf("sigmask: pselect6 mask not restored old=%#llx\n",
           (unsigned long long)old);
    return 1;
  }
  return 0;
}

static int test_ppoll_timeout(void) {
  uint64_t mask = 1ULL << (SIGUSR2 - 1);
  struct pollfd pfd = { -1, 0, 0 };
  struct timespec ts = { 0, 100000000 };  // 100ms
  long n = sys_ppoll(&pfd, 1, &ts, &mask, sizeof mask);
  if (n != 0) {
    printf("sigmask: ppoll timeout n=%ld errno=%d\n", n, errno);
    return 1;
  }
  return 0;
}

/* Leaf: arm a parent-death signal, park in pselect6 with that signal masked,
 * then report what pselect6 returned. SIGCHLD is used because its host
 * disposition is "ignore", so the restored-and-delivered signal does not
 * terminate the leaf before it reports. */
static void leaf_run(int ready_fd) {
  if (sys_prctl(1 /*PR_SET_PDEATHSIG*/, SIGCHLD) != 0) {
    write_result("prctl_failed");
    _exit(2);
  }
  uint64_t mask = 1ULL << (SIGCHLD - 1);
  struct { uint64_t ss; uint64_t len; } sigarg = { (uint64_t)&mask, sizeof mask };
  struct timespec ts = { 10, 0 };
  char c = 'r';
  write(ready_fd, &c, 1);  // tell the middle process we are parked-and-ready
  long n = sys_pselect6(0, NULL, NULL, NULL, &ts, &sigarg);
  if (n == -4) {
    write_result("eintr");
    _exit(0);
  }
  char buf[64];
  snprintf(buf, sizeof buf, "n=%ld", n);
  write_result(buf);
  _exit(3);
}

/* Middle: fork the leaf, wait until it has armed + parked, then die. */
static void middle_run(void) {
  int fds[2];
  if (pipe(fds) != 0) {
    write_result("pipe_failed");
    _exit(4);
  }
  pid_t leaf = fork();
  if (leaf < 0) {
    write_result("leaf_fork_failed");
    _exit(5);
  }
  if (leaf == 0) {
    close(fds[0]);
    leaf_run(fds[1]);
    _exit(6);  // not reached
  }
  close(fds[1]);
  char c;
  ssize_t got = read(fds[0], &c, 1);  // wait for the leaf to arm + park
  (void)got;
  close(fds[0]);
  _exit(0);  // parent death fires the leaf's pdeath signal
}

static int read_result(char* out, size_t cap) {
  int f = open("/tmp/sigmask_result", O_RDONLY);
  if (f < 0) return 0;
  ssize_t n = read(f, out, cap - 1);
  close(f);
  if (n <= 0) return 0;
  out[n] = 0;
  return 1;
}

int main(void) {
  if (test_pselect6_timeout() != 0) return 1;
  if (test_ppoll_timeout() != 0) return 1;
  puts("SIGMASK_GUEST_OK");
  fflush(stdout);

  pid_t middle = fork();
  if (middle < 0) {
    printf("sigmask: middle fork failed errno=%d\n", errno);
    return 1;
  }
  if (middle == 0) {
    middle_run();
    _exit(7);  // not reached
  }
  waitpid(middle, NULL, 0);  // reap the middle process (init must stay alive)

  // Poll for the leaf's result. The leaf writes "eintr" when pselect6 returns
  // EINTR; otherwise it writes "n=..." after its 10s timeout.
  char result[64] = {0};
  for (int i = 0; i < 150; i++) {
    if (read_result(result, sizeof result) && result[0] != 0) break;
    usleep(100000);
  }
  if (strcmp(result, "eintr") == 0) {
    puts("SIGMASK_EINTR_OK");
    return 0;
  }
  printf("sigmask: EINTR path failed result=%s\n", result[0] ? result : "(timeout)");
  return 1;
}
