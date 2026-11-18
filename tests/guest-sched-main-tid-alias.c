// Scheduler identity proof across a fork: the child's main TID is its PID.
#include <stdint.h>

struct sched_attr_v0 {
  uint32_t size, policy;
  uint64_t flags;
  int32_t nice;
  uint32_t priority;
  uint64_t runtime, deadline, period;
};
static struct sched_attr_v0 attr;
static struct sched_attr_v0 not_guest;

static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}

static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_SCHED_MAIN_TID_ALIAS_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}

static int check_scheduler(long pid) {
  int32_t priority = -1;
  uint64_t mask = 0;
  uint64_t unrestricted = UINT64_MAX;
  if (sc5(120, pid, 0, 0, 0, 0) != 0) return 1;
  if (sc5(121, pid, (long)&priority, 0, 0, 0) != 0 || priority) return 2;
  if (sc5(118, pid, (long)&priority, 0, 0, 0) != 0) return 3;
  if (sc5(119, pid, 0, (long)&priority, 0, 0) != 0) return 4;
  if (sc5(122, pid, 8, (long)&unrestricted, 0, 0) != 0) return 5;
  if (sc5(123, pid, 8, (long)&mask, 0, 0) != 8 || !mask) return 6;
  if (sc5(127, pid, (long)&mask, 0, 0, 0) != -95) return 7;
  if (sc5(275, pid, (long)&attr, sizeof attr, 0, 0) != 0 ||
      attr.size != sizeof attr || attr.policy) return 8;
  if (sc5(274, pid, (long)&attr, 0, 0, 0) != 0) return 9;
  return 0;
}

static long decimal(const char *s) {
  long n = 0;
  if (!s || !*s) return -1;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9') return -1;
    n = n * 10 + (*s - '0');
  }
  return n;
}

__attribute__((noreturn)) void start_c(unsigned long *stack) {
  long init_host_pid = stack[0] > 1 ? decimal(((char **)(stack + 1))[1]) : -1;
  long parent_pid = sc5(172, 0, 0, 0, 0, 0);
  long parent_tid = sc5(178, 0, 0, 0, 0, 0);
  if (parent_pid <= 0 || parent_tid != parent_pid) finish(1);
  if (init_host_pid > 1) {
    not_guest.size = sizeof not_guest;
    if (parent_pid != 1 || init_host_pid == parent_pid) finish(22);
    if (sc5(275, init_host_pid, (long)&not_guest, sizeof not_guest, 0, 0) != -3)
      finish(23);
    if (sc5(274, init_host_pid, (long)&not_guest, 0, 0, 0) != -3)
      finish(24);
  }
  if (check_scheduler(parent_pid)) finish(2);
  long child = sc5(220, 17, 0, 0, 0, 0); // clone(SIGCHLD), no CLONE_VM
  if (child == 0) {
    long pid = sc5(172, 0, 0, 0, 0, 0);
    long tid = sc5(178, 0, 0, 0, 0, 0);
    if (pid <= 0 || pid == parent_pid || tid != pid) finish(10);
    int error = check_scheduler(pid);
    if (error) finish(10 + error);
    finish(0);
  }
  if (child < 0) finish(20);
  int status = -1;
  if (sc5(260, child, (long)&status, 0, 0, 0) != child || status != 0)
    finish(21);
  finish(0);
}

__attribute__((naked)) void _start(void) {
  __asm__ volatile("mov x0, sp\n\tb start_c");
}
