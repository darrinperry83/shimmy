/* Compile with zig cc -target aarch64-linux-musl -static and run in an isolated root. */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc == 3 && argv[1][0] == 'E') {
    sigset_t awaited;
    sigemptyset(&awaited);
    sigaddset(&awaited, SIGTERM);
    int signo = sigwaitinfo(&awaited, NULL);
    char byte = signo == SIGTERM ? 'E' : 'F';
    write(atoi(argv[2]), &byte, 1);
    return signo == SIGTERM ? 0 : 30;
  }
  if (prctl(PR_SET_PDEATHSIG, 65) != -1 || errno != EINVAL) return 16;
  if (prctl(PR_SET_PDEATHSIG, SIGTERM)) return 17;
  if (prctl(PR_SET_PDEATHSIG, 0)) return 20;
  struct sigaction unsupported = {0}, original = {0};
  unsupported.sa_handler = (void (*)(int))2;
  if (sigaction(SIGTERM, &unsupported, &original)) return 39;
  if (prctl(PR_SET_PDEATHSIG, SIGTERM) != -1 || errno != ENOSYS) return 40;
  if (sigaction(SIGTERM, &original, NULL)) return 41;
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
  int async_result[2], async_ready[2];
  if (pipe(async_result) || pipe(async_ready)) return 21;
  intermediate = fork();
  if (intermediate < 0) return 22;
  if (intermediate == 0) {
    close(async_result[0]);
    pid_t child = fork();
    if (child < 0) _exit(23);
    if (child == 0) {
      close(async_ready[0]);
      if (prctl(PR_SET_PDEATHSIG, SIGTERM)) _exit(24);
      byte = 'R';
      write(async_ready[1], &byte, 1);
      close(async_ready[1]);
      struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
      nanosleep(&pause, NULL);
      byte = 'B';
      write(async_result[1], &byte, 1);
      sigprocmask(SIG_UNBLOCK, &set, NULL);
      byte = 'X'; // parent-death signal must terminate before this write
      write(async_result[1], &byte, 1);
      _exit(25);
    }
    char ready;
    close(async_ready[1]);
    if (read(async_ready[0], &ready, 1) != 1) _exit(26);
    _exit(0);
  }
  close(async_result[1]); close(async_ready[1]); close(async_ready[0]);
  if (waitpid(intermediate, &status, 0) != intermediate || !WIFEXITED(status)) return 27;
  char observed[2] = {0};
  ssize_t count = read(async_result[0], observed, sizeof observed);
  if (count != 1 || observed[0] != 'B') return 28;
  if (read(async_result[0], observed, 1) != 0) return 29;
  int exec_result[2], exec_ready[2];
  if (pipe(exec_result) || pipe(exec_ready)) return 31;
  intermediate = fork();
  if (intermediate < 0) return 32;
  if (intermediate == 0) {
    close(exec_result[0]);
    pid_t child = fork();
    if (child < 0) _exit(33);
    if (child == 0) {
      close(exec_ready[0]);
      if (prctl(PR_SET_PDEATHSIG, SIGTERM)) _exit(34);
      byte = 'R'; write(exec_ready[1], &byte, 1); close(exec_ready[1]);
      char fdarg[24]; snprintf(fdarg, sizeof fdarg, "%d", exec_result[1]);
      execl("/signal-test", "signal-test", "E", fdarg, (char*)NULL);
      _exit(35);
    }
    close(exec_ready[1]);
    if (read(exec_ready[0], &byte, 1) != 1) _exit(36);
    _exit(0);
  }
  close(exec_result[1]); close(exec_ready[0]); close(exec_ready[1]);
  if (waitpid(intermediate, &status, 0) != intermediate || !WIFEXITED(status)) return 37;
  if (read(exec_result[0], &byte, 1) != 1 || byte != 'E') return 38;
  int canceled[2], cancel_ready[2];
  if (pipe(canceled) || pipe(cancel_ready)) return 42;
  intermediate = fork();
  if (intermediate < 0) return 43;
  if (intermediate == 0) {
    close(canceled[0]);
    pid_t child = fork();
    if (child < 0) _exit(44);
    if (child == 0) {
      close(cancel_ready[0]);
      if (prctl(PR_SET_PDEATHSIG, SIGTERM) || prctl(PR_SET_PDEATHSIG, 0)) _exit(45);
      byte = 'R'; write(cancel_ready[1], &byte, 1); close(cancel_ready[1]);
      struct timespec pause = {.tv_sec = 0, .tv_nsec = 400000000};
      nanosleep(&pause, NULL);
      byte = 'C'; write(canceled[1], &byte, 1);
      _exit(0);
    }
    close(cancel_ready[1]);
    if (read(cancel_ready[0], &byte, 1) != 1) _exit(46);
    _exit(0);
  }
  close(canceled[1]); close(cancel_ready[0]); close(cancel_ready[1]);
  if (waitpid(intermediate, &status, 0) != intermediate || !WIFEXITED(status)) return 47;
  if (read(canceled[0], &byte, 1) != 1 || byte != 'C') return 48;
  puts("PARENT_DEATH_SIGNAL_PASS");
  return 0;
}
