#include "uffd_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

enum { UFFD_PAGE = 16384, UFFD_MAX_RANGES = 32, UFFD_MAX_FAULTS = 64 };
typedef struct { uint64_t start, end; } uffd_range;
typedef struct { uint64_t sequence, address; int state; } uffd_pending;

struct uffd_backend {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  int notify[2];
  uffd_range ranges[UFFD_MAX_RANGES];
  unsigned nranges;
  uffd_pending pending[UFFD_MAX_FAULTS];
  uffd_backend_event events[UFFD_MAX_FAULTS];
  unsigned nevents, active;
  uint64_t next_sequence;
  int closing;
};

static int valid_range(uint64_t start, uint64_t length) {
  return start && length && !(start & (UFFD_PAGE - 1)) &&
         !(length & (UFFD_PAGE - 1)) && start <= UINT64_MAX - length;
}

static int contained(const uffd_backend *b, uint64_t address) {
  for (unsigned i = 0; i < b->nranges; ++i)
    if (address >= b->ranges[i].start && address < b->ranges[i].end) return 1;
  return 0;
}

static void discard_notifications(uffd_backend *b) {
  char c[64];
  while (read(b->notify[0], c, sizeof c) > 0) { }
}

uffd_backend *uffd_backend_create(void) {
  uffd_backend *b = calloc(1, sizeof *b);
  if (!b) return NULL;
  b->notify[0] = b->notify[1] = -1;
  if (pthread_mutex_init(&b->lock, NULL)) {
    free(b);
    errno = ENOMEM;
    return NULL;
  }
  if (pthread_cond_init(&b->changed, NULL)) {
    pthread_mutex_destroy(&b->lock);
    free(b);
    errno = ENOMEM;
    return NULL;
  }
  if (pipe(b->notify) || fcntl(b->notify[0], F_SETFL, O_NONBLOCK) < 0 ||
      fcntl(b->notify[1], F_SETFL, O_NONBLOCK) < 0 ||
      fcntl(b->notify[0], F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl(b->notify[1], F_SETFD, FD_CLOEXEC) < 0) {
    int saved = errno;
    if (b->notify[0] >= 0) close(b->notify[0]);
    if (b->notify[1] >= 0) close(b->notify[1]);
    pthread_cond_destroy(&b->changed);
    pthread_mutex_destroy(&b->lock);
    free(b);
    errno = saved;
    return NULL;
  }
  return b;
}

int uffd_backend_poll_fd(uffd_backend *b) {
  if (!b) { errno = EINVAL; return -1; }
  return b->notify[0];
}

int uffd_backend_register(uffd_backend *b, uint64_t start, uint64_t length) {
  if (!b || !valid_range(start, length)) { errno = EINVAL; return -1; }
  pthread_mutex_lock(&b->lock);
  if (b->closing) { pthread_mutex_unlock(&b->lock); errno = EBADF; return -1; }
  uint64_t end = start + length;
  for (unsigned i = 0; i < b->nranges; ++i)
    if (start < b->ranges[i].end && end > b->ranges[i].start) {
      pthread_mutex_unlock(&b->lock); errno = EBUSY; return -1;
    }
  if (b->nranges == UFFD_MAX_RANGES) {
    pthread_mutex_unlock(&b->lock); errno = ENOSPC; return -1;
  }
  b->ranges[b->nranges++] = (uffd_range){start, end};
  pthread_mutex_unlock(&b->lock);
  return 0;
}

int uffd_backend_unregister(uffd_backend *b, uint64_t start, uint64_t length) {
  if (!b || !valid_range(start, length)) { errno = EINVAL; return -1; }
  pthread_mutex_lock(&b->lock);
  unsigned ix = 0;
  while (ix < b->nranges && (b->ranges[ix].start != start ||
                             b->ranges[ix].end != start + length)) ++ix;
  if (ix == b->nranges) { pthread_mutex_unlock(&b->lock); errno = ENOENT; return -1; }
  for (unsigned j = ix + 1; j < b->nranges; ++j) b->ranges[j - 1] = b->ranges[j];
  --b->nranges;
  for (unsigned j = 0; j < UFFD_MAX_FAULTS; ++j)
    if (b->pending[j].state == 1 && b->pending[j].address >= start &&
        b->pending[j].address < start + length) b->pending[j].state = 3;
  unsigned out = 0;
  for (unsigned j = 0; j < b->nevents; ++j)
    if (b->events[j].address < start || b->events[j].address >= start + length)
      b->events[out++] = b->events[j];
  b->nevents = out;
  discard_notifications(b);
  for (unsigned j = 0; j < b->nevents; ++j) {
    char c = 1;
    (void)write(b->notify[1], &c, 1);
  }
  pthread_cond_broadcast(&b->changed);
  pthread_mutex_unlock(&b->lock);
  return 0;
}

int uffd_backend_report_fault(uffd_backend *b, uint64_t address, uint64_t flags) {
  if (!b) { errno = EINVAL; return -1; }
  pthread_mutex_lock(&b->lock);
  if (b->closing || !contained(b, address)) {
    pthread_mutex_unlock(&b->lock); return 0;
  }
  unsigned ix = 0;
  while (ix < UFFD_MAX_FAULTS && b->pending[ix].state) ++ix;
  if (ix == UFFD_MAX_FAULTS || b->nevents == UFFD_MAX_FAULTS) {
    pthread_mutex_unlock(&b->lock); errno = EAGAIN; return -1;
  }
  uint64_t seq = ++b->next_sequence;
  b->pending[ix] = (uffd_pending){seq, address, 1};
  b->events[b->nevents++] = (uffd_backend_event){seq, address, flags};
  ++b->active;
  char c = 1;
  if (write(b->notify[1], &c, 1) != 1) {
    --b->active; --b->nevents; b->pending[ix].state = 0;
    pthread_mutex_unlock(&b->lock); errno = EIO; return -1;
  }
  while (b->pending[ix].state == 1) pthread_cond_wait(&b->changed, &b->lock);
  int completed = b->pending[ix].state == 2;
  b->pending[ix].state = 0;
  --b->active;
  pthread_cond_broadcast(&b->changed);
  pthread_mutex_unlock(&b->lock);
  return completed;
}

int uffd_backend_read_event(uffd_backend *b, uffd_backend_event *out) {
  if (!b || !out) { errno = EINVAL; return -1; }
  pthread_mutex_lock(&b->lock);
  if (!b->nevents) { pthread_mutex_unlock(&b->lock); errno = EAGAIN; return 0; }
  *out = b->events[0];
  for (unsigned i = 1; i < b->nevents; ++i) b->events[i - 1] = b->events[i];
  --b->nevents;
  char c;
  if (read(b->notify[0], &c, 1) != 1) {
    pthread_mutex_unlock(&b->lock);
    errno = EIO; // internal queue/readiness invariant, never abort a guest
    return -1;
  }
  pthread_mutex_unlock(&b->lock);
  return 1;
}

int uffd_backend_complete(uffd_backend *b, uint64_t sequence) {
  if (!b || !sequence) { errno = EINVAL; return -1; }
  pthread_mutex_lock(&b->lock);
  for (unsigned i = 0; i < UFFD_MAX_FAULTS; ++i)
    if (b->pending[i].state == 1 && b->pending[i].sequence == sequence) {
      b->pending[i].state = 2;
      pthread_cond_broadcast(&b->changed);
      pthread_mutex_unlock(&b->lock);
      return 0;
    }
  pthread_mutex_unlock(&b->lock);
  errno = ENOENT;
  return -1;
}

void uffd_backend_destroy(uffd_backend *b) {
  if (!b) return;
  pthread_mutex_lock(&b->lock);
  b->closing = 1;
  for (unsigned i = 0; i < UFFD_MAX_FAULTS; ++i)
    if (b->pending[i].state == 1) b->pending[i].state = 3;
  pthread_cond_broadcast(&b->changed);
  while (b->active) pthread_cond_wait(&b->changed, &b->lock);
  pthread_mutex_unlock(&b->lock);
  close(b->notify[0]); close(b->notify[1]);
  pthread_cond_destroy(&b->changed);
  pthread_mutex_destroy(&b->lock);
  free(b);
}
