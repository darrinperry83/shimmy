#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static char parent_mem[16384];
static char child_mem[16384];
static int child_result;

static void *child(void *unused) {
  (void)unused;
  stack_t old = {0}, next = {.ss_sp = child_mem, .ss_size = sizeof child_mem};
  if (syscall(SYS_sigaltstack, NULL, &old) || old.ss_flags != SS_DISABLE) {
    child_result = 1; return NULL;
  }
  if (syscall(SYS_sigaltstack, &next, NULL) ||
      syscall(SYS_sigaltstack, NULL, &old) || old.ss_sp != child_mem) {
    child_result = 2;
  }
  return NULL;
}

int main(void) {
  stack_t old = {0}, next = {.ss_sp = parent_mem, .ss_size = sizeof parent_mem};
  pthread_t thread;
  if (syscall(SYS_sigaltstack, &next, NULL)) return 1;
  if (pthread_create(&thread, NULL, child, NULL)) return 2;
  if (pthread_join(thread, NULL) || child_result) return 3 + child_result;
  if (syscall(SYS_sigaltstack, NULL, &old) || old.ss_sp != parent_mem)
    return 6;
  puts("GUEST_SIGALTSTACK_THREADS_OK");
  return 0;
}
