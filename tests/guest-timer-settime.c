// Raw ARM64 probe for the staged SIGEV_NONE timer_settime contract.
#include <stdint.h>

struct ts { int64_t sec, nsec; };
struct its { struct ts interval, value; };
struct lx_sigevent {
  uint64_t value;
  int32_t signo, notify;
  unsigned char rest[48];
};
static struct lx_sigevent ev = {.notify = 1};

static long sc1(long nr, long a) {
  register long x0 __asm__("x0") = a, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}
static long sc2(long nr, long a, long b) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
  return x0;
}
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int status) {
  static const char ok[] = "timer_settime staged ABI PASS\n";
  if (!status) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc1(93, status);
  for (;;) {}
}
static long set(int id, long flags, const struct its* n, struct its* o) {
  return sc4(110, id, flags, (long)n, (long)o);
}
static int valid_remaining(const struct ts* t, int64_t limit_ns) {
  return (t->sec > 0 || t->nsec > 0) &&
         t->sec * 1000000000 + t->nsec <= limit_ns;
}
void _start(void) {
  int32_t id = -1;
  if (sc3(107, 1, (long)&ev, (long)&id) || id < 0) finish(1);
  struct its arm = {.value = {0, 120000000}}, old = {{0}};
  if (set(id + 100, 0, &arm, 0) != -22) finish(2);
  if (set(id, 0, 0, 0) != -22) finish(3);
  if (set(id, 0, (void*)1, 0) != -14) finish(4);
  struct its bad = {.value = {0, 1000000000}};
  if (set(id, 0, &bad, 0) != -22) finish(5);
  if (set(id, 2, &arm, 0) != -95) finish(6);
  if (set(id, 0, &arm, &old) || old.value.sec || old.value.nsec) finish(7);
  struct ts sleep_for = {0, 30000000};
  if (sc2(101, (long)&sleep_for, 0)) finish(8);
  struct its disarm = {{0}};
  if (set(id, 0, &disarm, &old) || !valid_remaining(&old.value, 120000000)) finish(9);
  if (set(id, 0, &disarm, &old) || old.value.sec || old.value.nsec) finish(10);

  struct ts now;
  if (sc2(113, 1, (long)&now)) finish(11);
  arm.value = now;
  arm.value.nsec += 80000000;
  if (arm.value.nsec >= 1000000000) { arm.value.sec++; arm.value.nsec -= 1000000000; }
  if (set(id, 1, &arm, 0) || set(id, 0, &disarm, &old) ||
      !valid_remaining(&old.value, 80000000)) finish(12);

  arm.interval = (struct ts){0, 50000000};
  arm.value = (struct ts){0, 20000000};
  if (set(id, 0, &arm, 0)) finish(13);
  sleep_for.nsec = 80000000;
  if (sc2(101, (long)&sleep_for, 0)) finish(14);
  if (set(id, 0, &disarm, &old) ||
      !valid_remaining(&old.value, 50000000) || old.interval.nsec != 50000000)
    finish(15);

  arm.interval = (struct ts){0};
  arm.value = (struct ts){0, 100000000};
  if (set(id, 0, &arm, (void*)1) != -14) finish(16);
  if (set(id, 0, &disarm, &old) || !valid_remaining(&old.value, 100000000))
    finish(17); // Linux reports EFAULT after the new timer was installed.
  // A huge relative duration must not overflow the host deadline or become
  // an immediately expired timer.
  arm.value = (struct ts){INT64_MAX, 999999999};
  if (set(id, 0, &arm, 0) || set(id, 0, &disarm, &old) || old.value.sec < 1000000)
    finish(19);
  if (sc1(111, id)) finish(18);

  int32_t realtime_id = -1;
  if (sc3(107, 0, (long)&ev, (long)&realtime_id)) finish(20);
  if (sc2(113, 0, (long)&now)) finish(21);
  arm.value = now;
  arm.value.nsec += 80000000;
  if (arm.value.nsec >= 1000000000) { arm.value.sec++; arm.value.nsec -= 1000000000; }
  if (set(realtime_id, 1, &arm, 0) ||
      set(realtime_id, 0, &disarm, &old) ||
      !valid_remaining(&old.value, 80000000)) finish(22);
  if (sc1(111, realtime_id)) finish(23);
  finish(0);
}
