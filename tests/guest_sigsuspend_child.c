/* Compile as aarch64-linux-musl static PIE and run in an isolated Shimmy root. */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void child_signal(int sig) { (void)sig; }
static long elapsed_ms(struct timespec a, struct timespec b) {
  return (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
}
int main(void) {
  sigset_t blocked, empty, after;
  sigemptyset(&blocked); sigaddset(&blocked, SIGCHLD); sigemptyset(&empty);
  struct sigaction action = {.sa_handler = child_signal};
  if (sigaction(SIGCHLD, &action, NULL)) return 1;
  if (sigprocmask(SIG_BLOCK, &blocked, NULL)) return 2;
  if (syscall(SYS_rt_sigsuspend, &empty, 7) != -1 || errno != EINVAL) return 3;
  if (syscall(SYS_rt_sigsuspend, NULL, 8) != -1 || errno != EFAULT) return 4;
  if (syscall(SYS_rt_sigsuspend, (void *)(uintptr_t)1, 8) != -1 || errno != EFAULT) return 11;
  pid_t child = fork();
  if (child < 0) return 5;
  if (child == 0) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 200000000};
    nanosleep(&pause, NULL);
    _exit(7);
  }
  if (waitpid(child, NULL, WNOHANG) != 0) return 6;
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  if (sigsuspend(&empty) != -1 || errno != EINTR) return 7;
  clock_gettime(CLOCK_MONOTONIC, &end);
  if (elapsed_ms(start, end) < 150) return 8; // no synthetic early wake
  if (sigprocmask(SIG_SETMASK, NULL, &after) || !sigismember(&after, SIGCHLD)) return 9;
  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 7) return 10;
  puts("SIGSUSPEND_CHILD_PASS");
  return 0;
}
