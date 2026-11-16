/* Compile with zig cc -target aarch64-linux-musl -static and run in an isolated root. */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(void) {
  if (prctl(PR_SET_PDEATHSIG, 65) != -1 || errno != EINVAL) return 16;
  if (prctl(PR_SET_PDEATHSIG, SIGTERM) != -1 || errno != ENOSYS) return 17;
  sigset_t set, pending;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  if (sigprocmask(SIG_BLOCK, &set, NULL)) return 1;
  struct timespec short_wait = {.tv_sec = 0, .tv_nsec = 1000000};
  if (sigtimedwait(&set, NULL, &short_wait) != -1 || errno != EAGAIN) return 2;
  struct timespec bad_wait = {.tv_sec = 0, .tv_nsec = 1000000000};
  if (sigtimedwait(&set, NULL, &bad_wait) != -1 || errno != EINVAL) return 18;
  if (prctl(PR_SET_PDEATHSIG, SIGTERM)) return 3;
  int got = -1;
  if (prctl(PR_GET_PDEATHSIG, &got) || got != SIGTERM) return 4;
  int result[2];
  if (pipe(result)) return 5;
  pid_t intermediate = fork();
  if (intermediate < 0) return 6;
  if (intermediate == 0) {
    close(result[0]);
    int ready[2];
    if (pipe(ready)) _exit(7);
    pid_t waiter = fork();
    if (waiter < 0) _exit(8);
    if (waiter == 0) {
      close(ready[0]);
      if (prctl(PR_SET_PDEATHSIG, SIGTERM)) _exit(9);
      char byte = 'r';
      if (write(ready[1], &byte, 1) != 1) _exit(10);
      close(ready[1]);
      struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
      nanosleep(&pause, NULL);
      if (sigpending(&pending) || !sigismember(&pending, SIGTERM)) _exit(11);
      int signo = sigwaitinfo(&set, NULL);
      if (sigpending(&pending) || sigismember(&pending, SIGTERM)) _exit(19);
      byte = signo == SIGTERM ? 'P' : 'F';
      write(result[1], &byte, 1);
      _exit(signo == SIGTERM ? 0 : 12);
    }
    close(ready[1]);
    char byte;
    if (read(ready[0], &byte, 1) != 1) _exit(13);
    _exit(0); // waiter must observe THIS parent's death
  }
  close(result[1]);
  int status = 0;
  if (waitpid(intermediate, &status, 0) != intermediate || !WIFEXITED(status)) return 14;
  char byte = 0;
  if (read(result[0], &byte, 1) != 1 || byte != 'P') return 15;
  puts("PARENT_DEATH_SIGNAL_PASS");
  return 0;
}
