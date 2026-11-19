#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

// The host Mac has been up ~5 days, so a guest clock that leaks the host's CLOCK_UPTIME_RAW
// (or CLOCK_MONOTONIC_RAW) would report ~432000 seconds. Anything under an hour proves the
// epoch subtraction rebased the clock to guest boot.
#define MAX_GUEST_UPTIME_SEC 3600

static long long ns_of(struct timespec t) {
  return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

// clock_gettime(clock) must be small (< 1h) and non-decreasing across two reads.
static int check_mono(clockid_t clock, const char* name) {
  struct timespec a, b;
  if (clock_gettime(clock, &a) != 0) {
    fprintf(stderr, "clock_gettime(%s) failed: %s\n", name, strerror(errno));
    return 1;
  }
  if (a.tv_sec < 0 || a.tv_nsec < 0 || a.tv_nsec >= 1000000000L) {
    fprintf(stderr, "%s: malformed timespec {%lld,%lld}\n", name,
            (long long)a.tv_sec, (long long)a.tv_nsec);
    return 2;
  }
  if (a.tv_sec >= MAX_GUEST_UPTIME_SEC) {
    fprintf(stderr, "%s = %lld sec (host-uptime leaked)\n", name, (long long)a.tv_sec);
    return 3;
  }
  struct timespec sl = {0, 2000000};   // 2 ms: enough for any monotonic clock to advance
  nanosleep(&sl, NULL);
  if (clock_gettime(clock, &b) != 0) {
    fprintf(stderr, "clock_gettime(%s) #2 failed: %s\n", name, strerror(errno));
    return 4;
  }
  if (ns_of(b) < ns_of(a)) {
    fprintf(stderr, "%s went backwards: %lld -> %lld\n", name, ns_of(a), ns_of(b));
    return 5;
  }
  printf("%s: %lld.%09lld -> %lld.%09lld\n", name,
         (long long)a.tv_sec, (long long)a.tv_nsec,
         (long long)b.tv_sec, (long long)b.tv_nsec);
  return 0;
}

int main(void) {
  if (check_mono(CLOCK_MONOTONIC, "CLOCK_MONOTONIC")) return 10;
  if (check_mono(CLOCK_BOOTTIME, "CLOCK_BOOTTIME")) return 11;

  struct sysinfo si;
  if (sysinfo(&si) != 0) {
    fprintf(stderr, "sysinfo failed: %s\n", strerror(errno));
    return 12;
  }
  if (si.uptime < 0 || si.uptime >= MAX_GUEST_UPTIME_SEC) {
    fprintf(stderr, "sysinfo uptime = %ld (host-uptime leaked)\n", (long)si.uptime);
    return 13;
  }
  printf("sysinfo uptime: %ld sec\n", (long)si.uptime);

  // CLOCK_REALTIME must remain the real wall clock (~2026 == epoch 1.7e9), not ~0.
  struct timespec rt;
  if (clock_gettime(CLOCK_REALTIME, &rt) != 0) {
    fprintf(stderr, "clock_gettime(CLOCK_REALTIME) failed: %s\n", strerror(errno));
    return 14;
  }
  if (rt.tv_sec < 1000000000LL) {
    fprintf(stderr, "CLOCK_REALTIME looks wrong: %lld sec\n", (long long)rt.tv_sec);
    return 15;
  }
  printf("CLOCK_REALTIME: %lld.%09lld\n", (long long)rt.tv_sec, (long long)rt.tv_nsec);

  puts("CLOCKFIX_OK");
  return 0;
}
