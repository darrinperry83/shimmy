#include "timerfd_shim.h"
#include <dispatch/dispatch.h>
#include <sys/socket.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_TIMERFD 32
#define L_NONBLOCK 0x800
#define L_CLOEXEC 0x80000
struct timer_slot {
  int fd, writer, clockid;
  dispatch_source_t source;
  linux_itimerspec spec;
  int64_t due_ns;
};
static struct timer_slot slots[MAX_TIMERFD];
static int count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static dispatch_queue_t queue;
static _Atomic int trace_arms, trace_fires;
static void trace_timer(const char* kind, int fd, int64_t value, int64_t interval) {
  const char* enabled = getenv("RDTIMERFD_TRACE");
  if (!enabled || !*enabled || *enabled == '0') return;
  if (!strcmp(kind, "arm-absolute") || !strcmp(kind, "arm-relative") ||
      !strcmp(kind, "schedule-now-and-delay")) {
    int n = atomic_fetch_add(&trace_arms, 1);
    if (n >= 20 && n % 1000 != 0) return;
  } else if (!strcmp(kind, "fire") && atomic_fetch_add(&trace_fires, 1) >= 50) return;
  fprintf(stderr, "[timerfd] %s fd=%d value_ns=%lld interval_ns=%lld\n",
          kind, fd, (long long)value, (long long)interval);
}

static int64_t ns(struct timespec t) { return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
static struct timespec from_ns(int64_t n) {
  struct timespec t = { n / 1000000000LL, n % 1000000000LL };
  return t;
}
static int64_t now_ns(int clockid) {
  struct timespec now;
  // The guest vDSO maps Linux CLOCK_MONOTONIC to macOS CLOCK_UPTIME_RAW.
  // Absolute timerfd deadlines must use the same epoch or frame timers fire
  // several seconds late on macOS (the two host clocks differ on this machine).
  clock_gettime(clockid == 0 ? CLOCK_REALTIME : CLOCK_UPTIME_RAW, &now);
  return ns(now);
}
static void fire(void* context) {
  struct timer_slot* slot = context;
  pthread_mutex_lock(&lock);
  // Dispatch may deliver a callback queued for an earlier arm after settime
  // has already replaced it. Ignore that callback until the new deadline.
  int64_t now = now_ns(slot->clockid);
  if (slot->fd < 0 || !slot->due_ns || now < slot->due_ns) {
    pthread_mutex_unlock(&lock);
    return;
  }
  uint64_t one = 1;
  int64_t interval = ns(slot->spec.it_interval);
  if (interval) {
    one += (uint64_t)((now - slot->due_ns) / interval);
    slot->due_ns += (int64_t)one * interval;
  } else {
    slot->due_ns = 0;
  }
  // An 8-byte datagram is atomic and makes the fd readable to poll/epoll.
  // This prototype supports Weston's one-shot frame timers; precise Linux
  // accumulation across missed periodic intervals remains to be implemented.
  trace_timer("fire", slot->fd, now, interval);
  (void)write(slot->writer, &one, sizeof one);
  pthread_mutex_unlock(&lock);
}
static void cancelled(void* context) {
  struct timer_slot* slot = context;
  if (slot->writer >= 0) { close(slot->writer); slot->writer = -1; }
}
static struct timer_slot* lookup(int fd) {
  for (int i = 0; i < count; i++) if (slots[i].fd == fd) return &slots[i];
  return NULL;
}
long timerfd_create_shim(int clockid, int flags) {
  if (clockid != 0 && clockid != 1) return -22;
  if (flags & ~(L_NONBLOCK | L_CLOEXEC)) return -22;
  pthread_mutex_lock(&lock);
  if (count >= MAX_TIMERFD) { pthread_mutex_unlock(&lock); return -24; }
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) < 0) {
    int rc = -errno; pthread_mutex_unlock(&lock); return rc;
  }
  fcntl(pair[1], F_SETFL, O_NONBLOCK);
  if (flags & L_NONBLOCK) fcntl(pair[0], F_SETFL, O_NONBLOCK);
  if (flags & L_CLOEXEC) { fcntl(pair[0], F_SETFD, FD_CLOEXEC); fcntl(pair[1], F_SETFD, FD_CLOEXEC); }
  if (!queue) queue = dispatch_queue_create("org.linux-shim.timerfd", DISPATCH_QUEUE_SERIAL);
  struct timer_slot* slot = &slots[count];
  memset(slot, 0, sizeof *slot);
  slot->fd = pair[0]; slot->writer = pair[1]; slot->clockid = clockid;
  slot->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
  if (!slot->source) { close(pair[0]); close(pair[1]); pthread_mutex_unlock(&lock); return -12; }
  dispatch_set_context(slot->source, slot);
  dispatch_source_set_event_handler_f(slot->source, fire);
  dispatch_source_set_cancel_handler_f(slot->source, cancelled);
  dispatch_source_set_timer(slot->source, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
  dispatch_resume(slot->source);
  count++;
  trace_timer("create", slot->fd, clockid, flags);
  pthread_mutex_unlock(&lock);
  return pair[0];
}
static void current_locked(struct timer_slot* slot, linux_itimerspec* current) {
  memset(current, 0, sizeof *current);
  current->it_interval = slot->spec.it_interval;
  if (slot->due_ns) {
    int64_t left = slot->due_ns - now_ns(slot->clockid);
    if (left > 0) current->it_value = from_ns(left);
  }
}
long timerfd_settime_shim(int fd, int flags, const linux_itimerspec* next, linux_itimerspec* old) {
  if (!next) return -14;
  if (flags & ~1) return -22;
  if (next->it_value.tv_sec < 0 || next->it_value.tv_nsec < 0 || next->it_value.tv_nsec >= 1000000000 ||
      next->it_interval.tv_sec < 0 || next->it_interval.tv_nsec < 0 || next->it_interval.tv_nsec >= 1000000000) return -22;
  pthread_mutex_lock(&lock);
  struct timer_slot* slot = lookup(fd);
  if (!slot) { pthread_mutex_unlock(&lock); return -9; }
  if (old) current_locked(slot, old);
  // Linux timerfd_settime discards unread expirations. A queued datagram from
  // the previous arm otherwise keeps epoll readable and can spin Weston.
  uint64_t discarded;
  while (recv(slot->fd, &discarded, sizeof discarded, MSG_DONTWAIT) == sizeof discarded) {}
  slot->spec = *next;
  int64_t value = ns(next->it_value), interval = ns(next->it_interval);
  trace_timer((flags & 1) ? "arm-absolute" : "arm-relative", fd, value, interval);
  if (!value) {
    slot->due_ns = 0;
    dispatch_source_set_timer(slot->source, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
  } else {
    int64_t now = now_ns(slot->clockid);
    int64_t delay = (flags & 1) ? value - now : value;
    if (delay < 0) delay = 0;
    trace_timer("schedule-now-and-delay", fd, now, delay);
    slot->due_ns = now + delay;
    dispatch_source_set_timer(slot->source, dispatch_time(DISPATCH_TIME_NOW, delay),
                              interval ? (uint64_t)interval : DISPATCH_TIME_FOREVER, 1000000);
  }
  pthread_mutex_unlock(&lock);
  return 0;
}
long timerfd_gettime_shim(int fd, linux_itimerspec* current) {
  if (!current) return -14;
  pthread_mutex_lock(&lock);
  struct timer_slot* slot = lookup(fd);
  if (!slot) { pthread_mutex_unlock(&lock); return -9; }
  current_locked(slot, current);
  pthread_mutex_unlock(&lock);
  return 0;
}
void timerfd_shim_closed(int fd) {
  pthread_mutex_lock(&lock);
  struct timer_slot* slot = lookup(fd);
  if (slot) { trace_timer("close", fd, 0, 0); slot->fd = -1; dispatch_source_cancel(slot->source); slot->due_ns = 0; }
  pthread_mutex_unlock(&lock);
}
