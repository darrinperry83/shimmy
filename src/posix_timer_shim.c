// Linux POSIX timer registry. The first bounded stage supports only disarmed
// CLOCK_REALTIME / CLOCK_MONOTONIC timers with explicit SIGEV_NONE. The table
// is process-owned, shared by threads, and cleared in a fork child. Follow-on
// syscall branches must add arming, querying, overrun, and deletion here before
// this stage can be merged as a usable feature.
#include "posix_timer_shim.h"
#include "gmem.h"
#include "syscalls.h"            // syscalls_guest_monotonic_ns — guest monotonic epoch
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <limits.h>
#include <time.h>

enum { LX_EAGAIN = 11, LX_ENOMEM = 12, LX_EFAULT = 14,
       LX_EINVAL = 22, LX_EOPNOTSUPP = 95,
       LX_CLOCK_REALTIME = 0, LX_CLOCK_MONOTONIC = 1,
       LX_SIGEV_SIGNAL = 0, LX_SIGEV_NONE = 1,
       LX_SIGEV_THREAD = 2, LX_SIGEV_THREAD_ID = 4 };
enum { LX_TIMER_ABSTIME = 1 };

// ARM64 Linux __kernel_itimerspec: two signed 64-bit timespecs.
struct lx_itimerspec {
  int64_t interval_sec, interval_nsec;
  int64_t value_sec, value_nsec;
};
_Static_assert(sizeof(struct lx_itimerspec) == 32, "Linux ARM64 itimerspec size");

// Linux ARM64 sigevent_t is 64 bytes. Only the notify field is interpreted;
// the complete object is copied, matching the kernel's copy_from_user.
struct lx_sigevent {
  uint64_t value;
  int32_t signo;
  int32_t notify;
  unsigned char rest[48];
};
_Static_assert(sizeof(struct lx_sigevent) == 64, "Linux ARM64 sigevent size");

struct lx_posix_timer {
  struct lx_posix_timer* next;
  int32_t id;
  int32_t clockid;
  // Initially disarmed. Later family calls will own these fields under lock.
  int64_t interval_sec, interval_nsec;
  int64_t deadline_sec, deadline_nsec;
  // Linux runs relative CLOCK_REALTIME timers on a monotonic timebase, so
  // wall-clock adjustments cannot move an already-armed relative deadline.
  int relative_realtime;
};

static pthread_mutex_t g_timer_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_timer_once = PTHREAD_ONCE_INIT;
static int g_atfork_error;
static struct lx_posix_timer* g_timers;
static int64_t g_next_id;

static void timer_fork_prepare(void) { pthread_mutex_lock(&g_timer_mu); }
static void timer_fork_parent(void) { pthread_mutex_unlock(&g_timer_mu); }
static void timer_fork_child(void) {
  // POSIX timers are not inherited. Avoid free() here: the child may inherit
  // malloc's lock from a vanished thread. The copied list is reclaimed on exec
  // or exit, and is unreachable from the child's timer table.
  g_timers = NULL;
  g_next_id = 0;
  pthread_mutex_unlock(&g_timer_mu);
}
static void timer_init_once(void) {
  g_atfork_error = pthread_atfork(timer_fork_prepare, timer_fork_parent,
                                  timer_fork_child);
}

long posix_timer_shim_create(uint64_t clockid, uint64_t sigevent_addr,
                             uint64_t timer_id_addr) {
  if (clockid != LX_CLOCK_REALTIME && clockid != LX_CLOCK_MONOTONIC)
    return -LX_EINVAL;
  if (!sigevent_addr) return -LX_EOPNOTSUPP; // default SIGEV_SIGNAL not staged
  if (!timer_id_addr) return -LX_EFAULT;
  struct lx_sigevent event;
  if (gmem_guest_export_copyin(NULL, sigevent_addr, &event, sizeof event) != 0)
    return -LX_EFAULT;
  if (event.notify != LX_SIGEV_NONE) {
    if (event.notify == LX_SIGEV_SIGNAL || event.notify == LX_SIGEV_THREAD ||
        event.notify == LX_SIGEV_THREAD_ID) return -LX_EOPNOTSUPP;
    return -LX_EINVAL;
  }

  pthread_once(&g_timer_once, timer_init_once);
  if (g_atfork_error) return -LX_EAGAIN;
  struct lx_posix_timer* timer = calloc(1, sizeof *timer);
  if (!timer) return -LX_ENOMEM;

  pthread_mutex_lock(&g_timer_mu);
  if (g_next_id > INT_MAX) {
    pthread_mutex_unlock(&g_timer_mu);
    free(timer);
    return -LX_EAGAIN;
  }
  int32_t id = (int32_t)g_next_id;
  if (gmem_guest_copyout_checked(timer_id_addr, &id, sizeof id) != 0) {
    pthread_mutex_unlock(&g_timer_mu);
    free(timer);
    return -LX_EFAULT;
  }
  timer->id = id;
  timer->clockid = (int32_t)clockid;
  timer->next = g_timers;
  g_timers = timer;
  ++g_next_id;
  pthread_mutex_unlock(&g_timer_mu);
  return 0;
}

long posix_timer_shim_delete(uint64_t timer_id) {
  // Linux timer_t is an int. The registry belongs to this guest process;
  // a fork child starts with an empty table and cannot delete its parent's IDs.
  int32_t id = (int32_t)timer_id;
  pthread_once(&g_timer_once, timer_init_once);
  if (g_atfork_error) return -LX_EAGAIN;
  pthread_mutex_lock(&g_timer_mu);
  struct lx_posix_timer** link = &g_timers;
  while (*link && (*link)->id != id) link = &(*link)->next;
  if (!*link) {
    pthread_mutex_unlock(&g_timer_mu);
    return -LX_EINVAL;
  }
  struct lx_posix_timer* timer = *link;
  *link = timer->next;
  pthread_mutex_unlock(&g_timer_mu);
  free(timer);
  return 0;
}

static int valid_time(int64_t sec, int64_t nsec) {
  return sec >= 0 && nsec >= 0 && nsec < 1000000000;
}
static __int128 to_ns(int64_t sec, int64_t nsec) {
  return (__int128)sec * 1000000000 + nsec;
}
static void from_ns(__int128 ns, int64_t* sec, int64_t* nsec) {
  *sec = (int64_t)(ns / 1000000000);
  *nsec = (int64_t)(ns % 1000000000);
}
static __int128 max_ns(void) {
  return to_ns(INT64_MAX, 999999999);
}
static clockid_t timer_host_clock(const struct lx_posix_timer* timer) {
  return timer->clockid == LX_CLOCK_REALTIME && !timer->relative_realtime ?
         CLOCK_REALTIME : CLOCK_UPTIME_RAW;
}

// CLOCK_MONOTONIC timers express their deadlines in the guest's clock_gettime(CLOCK_MONOTONIC)
// time base, which is the host CLOCK_UPTIME_RAW rebased to guest boot. Read "now" in that same
// base so absolute monotonic deadlines compare against the same epoch the guest sees.
static void monotonic_now_ts(struct timespec* out) {
  uint64_t ns = syscalls_guest_monotonic_ns();
  out->tv_sec  = (time_t)(ns / 1000000000ULL);
  out->tv_nsec = (long)(ns % 1000000000ULL);
}

// Fill *out with the time base a timer's stored deadline is expressed in. Returns 0, or -1 with
// errno set if the host clock read fails.
static int timer_now(const struct lx_posix_timer* timer, struct timespec* out) {
  if (timer->clockid == LX_CLOCK_MONOTONIC) {
    monotonic_now_ts(out);
    return 0;
  }
  return clock_gettime(timer_host_clock(timer), out);
}

// This is also the readback operation required by timer_gettime. SIGEV_NONE
// timers are never placed on a host timer queue; Linux computes their expiry
// when queried, forwarding an expired periodic deadline to the next interval.
static void snapshot_locked(struct lx_posix_timer* timer, __int128 now,
                            struct lx_itimerspec* out) {
  *out = (struct lx_itimerspec){
    .interval_sec = timer->interval_sec,
    .interval_nsec = timer->interval_nsec,
  };
  __int128 deadline = to_ns(timer->deadline_sec, timer->deadline_nsec);
  if (!deadline) return;
  __int128 period = to_ns(timer->interval_sec, timer->interval_nsec);
  if (period && deadline <= now) {
    __int128 elapsed = now - deadline;
    __int128 advanced = deadline + (elapsed / period + 1) * period;
    deadline = advanced > max_ns() ? max_ns() : advanced;
    from_ns(deadline, &timer->deadline_sec, &timer->deadline_nsec);
  }
  if (deadline <= now) return;
  from_ns(deadline - now, &out->value_sec, &out->value_nsec);
}

long posix_timer_shim_settime(uint64_t timer_id, uint64_t flags,
                              uint64_t new_addr, uint64_t old_addr) {
  if (!new_addr) return -LX_EINVAL;
  struct lx_itimerspec setting;
  if (gmem_guest_export_copyin(NULL, new_addr, &setting, sizeof setting) != 0)
    return -LX_EFAULT;
  if (!valid_time(setting.interval_sec, setting.interval_nsec) ||
      !valid_time(setting.value_sec, setting.value_nsec)) return -LX_EINVAL;
  // Linux uses TIMER_ABSTIME; keep unknown flags explicitly outside the
  // supported subset rather than silently treating them as a relative timer.
  if (flags & ~((uint64_t)LX_TIMER_ABSTIME)) return -LX_EOPNOTSUPP;

  pthread_once(&g_timer_once, timer_init_once);
  if (g_atfork_error) return -LX_EAGAIN;
  pthread_mutex_lock(&g_timer_mu);
  struct lx_posix_timer* timer = g_timers;
  while (timer && timer->id != (int32_t)timer_id) timer = timer->next;
  if (!timer) {
    pthread_mutex_unlock(&g_timer_mu);
    return -LX_EINVAL;
  }
  struct timespec now_ts;
  // Read the old setting using its existing timebase before choosing the
  // new one. A realtime timer can switch between relative and absolute mode.
  if (timer_now(timer, &now_ts) != 0) {
    pthread_mutex_unlock(&g_timer_mu);
    return -LX_EINVAL;
  }
  struct lx_itimerspec old;
  snapshot_locked(timer, to_ns(now_ts.tv_sec, now_ts.tv_nsec), &old);

  __int128 value = to_ns(setting.value_sec, setting.value_nsec);
  int relative_realtime = value && timer->clockid == LX_CLOCK_REALTIME &&
                          !(flags & LX_TIMER_ABSTIME);
  if (value && !(flags & LX_TIMER_ABSTIME)) {
    // Relative CLOCK_MONOTONIC timers are rebased to the guest monotonic epoch; CLOCK_REALTIME
    // relative timers stay on an internal monotonic base so wall-clock adjustments cannot move
    // an already-armed relative deadline.
    if (timer->clockid == LX_CLOCK_MONOTONIC) {
      monotonic_now_ts(&now_ts);
    } else {
      clockid_t effective = relative_realtime ? CLOCK_UPTIME_RAW :
                            timer_host_clock(timer);
      if (clock_gettime(effective, &now_ts) != 0) {
        pthread_mutex_unlock(&g_timer_mu);
        return -LX_EINVAL;
      }
    }
    value += to_ns(now_ts.tv_sec, now_ts.tv_nsec);
    if (value > max_ns()) value = max_ns();
  }
  // Linux clears the interval when it_value is zero (disarm).
  timer->interval_sec = value ? setting.interval_sec : 0;
  timer->interval_nsec = value ? setting.interval_nsec : 0;
  timer->relative_realtime = relative_realtime;
  from_ns(value, &timer->deadline_sec, &timer->deadline_nsec);
  pthread_mutex_unlock(&g_timer_mu);

  // Linux copies old_setting after changing the timer. A bad old pointer
  // therefore reports EFAULT even though the new setting has taken effect.
  if (old_addr && gmem_guest_copyout_checked(old_addr, &old, sizeof old) != 0)
    return -LX_EFAULT;
  return 0;
}

long posix_timer_shim_gettime(uint64_t timer_id, uint64_t setting_addr) {
  pthread_once(&g_timer_once, timer_init_once);
  if (g_atfork_error) return -LX_EAGAIN;
  pthread_mutex_lock(&g_timer_mu);
  struct lx_posix_timer* timer = g_timers;
  while (timer && timer->id != (int32_t)timer_id) timer = timer->next;
  if (!timer) {
    pthread_mutex_unlock(&g_timer_mu);
    return -LX_EINVAL;
  }
  struct timespec now_ts;
  if (timer_now(timer, &now_ts) != 0) {
    pthread_mutex_unlock(&g_timer_mu);
    return -LX_EINVAL;
  }
  struct lx_itimerspec setting;
  snapshot_locked(timer, to_ns(now_ts.tv_sec, now_ts.tv_nsec), &setting);
  pthread_mutex_unlock(&g_timer_mu);
  return gmem_guest_copyout_checked(setting_addr, &setting, sizeof setting) == 0 ?
         0 : -LX_EFAULT;
}

long posix_timer_shim_getoverrun(uint64_t timer_id) {
  pthread_once(&g_timer_once, timer_init_once);
  if (g_atfork_error) return -LX_EAGAIN;
  pthread_mutex_lock(&g_timer_mu);
  struct lx_posix_timer* timer = g_timers;
  while (timer && timer->id != (int32_t)timer_id) timer = timer->next;
  if (!timer) {
    pthread_mutex_unlock(&g_timer_mu);
    return -LX_EINVAL;
  }
  // Linux returns it_overrun_last, which changes only when a timer signal is
  // delivered. SIGEV_NONE never queues or delivers one, even when a periodic
  // timer expires repeatedly. Its last-delivered overrun stays zero.
  pthread_mutex_unlock(&g_timer_mu);
  return 0;
}
