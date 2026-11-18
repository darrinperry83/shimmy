// Raw ARM64 probe for the staged SIGEV_NONE timer_gettime contract.
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
  static const char ok[] = "timer_gettime staged ABI PASS\n";
  if (!status) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc1(93, status);
  for (;;) {}
}
static int64_t ns(struct ts t) { return t.sec * 1000000000 + t.nsec; }
static int valid_remaining(struct ts t, int64_t cap) {
  return ns(t) > 0 && ns(t) <= cap;
}
static long get(int id, struct its* out) { return sc2(108, id, (long)out); }
static long set(int id, long flags, const struct its* in, struct its* old) {
  return sc4(110, id, flags, (long)in, (long)old);
}

void _start(void) {
  int32_t mono = -1, real = -1;
  if (sc3(107, 1, (long)&ev, (long)&mono) || mono < 0) finish(1);
  if (sc3(107, 0, (long)&ev, (long)&real) || real < 0) finish(2);
  struct its state = {{0}};
  if (get(mono, &state) || ns(state.value) || ns(state.interval)) finish(3);
  if (get(mono + 100, &state) != -22 || get(mono + 100, (void*)1) != -22)
    finish(4);
  if (get(mono, (void*)1) != -14) finish(5);

  struct its arm = {.value = {0, 160000000}};
  if (set(mono, 0, &arm, 0) || get(mono, &state) ||
      !valid_remaining(state.value, 160000000) || ns(state.interval)) finish(6);
  struct ts sleep_for = {0, 35000000};
  if (sc2(101, (long)&sleep_for, 0) || get(mono, &state) ||
      !valid_remaining(state.value, 125000000)) finish(7);
  sleep_for.nsec = 180000000;
  if (sc2(101, (long)&sleep_for, 0) || get(mono, &state) || ns(state.value))
    finish(8); // Expired SIGEV_NONE one-shot returns zero.

  arm = (struct its){.interval = {0, 40000000}, .value = {0, 25000000}};
  if (set(mono, 0, &arm, 0)) finish(9);
  sleep_for.nsec = 115000000;
  if (sc2(101, (long)&sleep_for, 0) || get(mono, &state) ||
      !valid_remaining(state.value, 40000000) || ns(state.interval) != 40000000)
    finish(10); // Periodic SIGEV_NONE advances on query.
  struct its disarm = {{0}};
  if (set(mono, 0, &disarm, 0) || get(mono, &state) ||
      ns(state.value) || ns(state.interval)) finish(11);

  arm = (struct its){.value = {0, 90000000}};
  if (set(real, 0, &arm, 0) || get(real, &state) ||
      !valid_remaining(state.value, 90000000)) finish(12);
  struct ts now;
  if (sc2(113, 0, (long)&now)) finish(13);
  arm.value = now;
  arm.value.nsec += 130000000;
  if (arm.value.nsec >= 1000000000) {
    arm.value.sec++;
    arm.value.nsec -= 1000000000;
  }
  struct its old;
  if (set(real, 1, &arm, &old) ||
      !valid_remaining(old.value, 90000000) ||
      get(real, &state) || !valid_remaining(state.value, 130000000))
    finish(14); // Mode switch must read old timer in its old timebase.
  if (sc1(111, mono) || sc1(111, real) || get(real, &state) != -22) finish(15);
  finish(0);
}
