// Linux POSIX timer registry. The first bounded stage supports only disarmed
// CLOCK_REALTIME / CLOCK_MONOTONIC timers with explicit SIGEV_NONE. The table
// is process-owned, shared by threads, and cleared in a fork child. Follow-on
// syscall branches must add arming, querying, overrun, and deletion here before
// this stage can be merged as a usable feature.
#include "posix_timer_shim.h"
#include "gmem.h"
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <limits.h>

enum { LX_EAGAIN = 11, LX_ENOMEM = 12, LX_EFAULT = 14,
       LX_EINVAL = 22, LX_EOPNOTSUPP = 95,
       LX_CLOCK_REALTIME = 0, LX_CLOCK_MONOTONIC = 1,
       LX_SIGEV_SIGNAL = 0, LX_SIGEV_NONE = 1,
       LX_SIGEV_THREAD = 2, LX_SIGEV_THREAD_ID = 4 };

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
