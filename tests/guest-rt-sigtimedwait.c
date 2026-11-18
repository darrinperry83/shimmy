/* Disposable ARM64 guest test of the bounded virtual signal wait. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long wait_sig(const uint64_t *set, siginfo_t *info, const struct timespec *timeout,
                     long size) {
  return syscall(SYS_rt_sigtimedwait, set, info, timeout, size);
}

int main(void) {
  uint64_t wanted = 1ULL << (SIGTERM - 1);
  struct timespec zero = {0, 0}, bad = {0, 1000000000}, two = {2, 0};
  siginfo_t info;
  memset(&info, 0x5a, sizeof info);
  if (wait_sig(NULL, NULL, &zero, 8) != -1 || errno != EFAULT) return 1;
  if (wait_sig((void *)1, NULL, &zero, 8) != -1 || errno != EFAULT) return 2;
  if (wait_sig(&wanted, NULL, (void *)1, 8) != -1 || errno != EFAULT) return 3;
  if (wait_sig(&wanted, NULL, &bad, 8) != -1 || errno != EINVAL) return 4;
  if (wait_sig(&wanted, &info, &zero, 7) != -1 || errno != EINVAL) return 5;
  if (wait_sig(&wanted, &info, &zero, 8) != -1 || errno != EAGAIN ||
      ((unsigned char *)&info)[0] != 0x5a) return 6;

  int ready[2], result[2];
  if (pipe(ready) || pipe(result)) return 7;
  pid_t middle = fork();
  if (middle < 0) return 8;
  if (middle == 0) {
    close(result[0]);
    pid_t waiter = fork();
    if (waiter < 0) _exit(9);
    if (waiter == 0) {
      close(ready[0]);
      uint64_t block = wanted;
      int result_code = 0;
      if (syscall(SYS_rt_sigprocmask, SIG_BLOCK, &block, NULL, 8) != 0)
        result_code = 10;
      if (!result_code && prctl(PR_SET_PDEATHSIG, SIGTERM) != 0)
        result_code = 11;
      if (write(ready[1], "R", 1) != 1) result_code = 12;
      struct timespec pause = {0, 100000000};
      nanosleep(&pause, NULL);
      if (!result_code && (wait_sig(&wanted, (void *)1, &two, 8) != -1 || errno != EFAULT))
        result_code = 13;
      if (!result_code && (wait_sig(&wanted, &info, &two, 8) != SIGTERM ||
                           info.si_signo != SIGTERM)) result_code = 14;
      (void)write(result[1], &result_code, sizeof result_code);
      _exit(result_code);
    }
    close(ready[1]); close(result[1]);
    char c;
    if (read(ready[0], &c, 1) != 1 || c != 'R') _exit(15);
    _exit(0); /* waiter must receive THIS parent's death signal */
  }
  close(ready[0]); close(ready[1]); close(result[1]);
  int status;
  pid_t reaped = waitpid(middle, &status, 0);
  if (reaped != middle || !WIFEXITED(status) || WEXITSTATUS(status)) {
    fprintf(stderr, "middle reaped=%d expected=%d status=%d errno=%d\n",
            (int)reaped, (int)middle, status, errno);
    return 16;
  }
  struct pollfd p = {.fd = result[0], .events = POLLIN};
  if (poll(&p, 1, 5000) != 1 || !(p.revents & POLLIN)) return 17;
  int result_code = 0;
  if (read(result[0], &result_code, sizeof result_code) != sizeof result_code || result_code)
    return result_code ? result_code : 18;
  puts("GUEST_RT_SIGTIMEDWAIT_OK");
  return 0;
}
