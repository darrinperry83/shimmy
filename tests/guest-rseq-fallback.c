#define _GNU_SOURCE
#include <errno.h>
#include <linux/rseq.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* A valid legacy-size registration must not succeed without preemption and
 * migration aborts. Check the guest's actual syscall path, not a host probe. */
enum { RSEQ_NR = 293, WORKERS = 4, INCREMENTS = 2000 };
static atomic_int fallback_count;
static atomic_int failures;

static int check_unsupported(unsigned int flags) {
  unsigned char area[32] __attribute__((aligned(32)));
  memset(area, 0xa5, sizeof area);
  errno = 0;
  long rc = syscall(RSEQ_NR, area, sizeof area, flags, 0xd428bc00u);
  if (rc != -1 || errno != ENOSYS) return -1;
  for (size_t i = 0; i < sizeof area; ++i)
    if (area[i] != 0xa5) return -2;
  return 0;
}

static void *worker(void *unused) {
  (void)unused;
  if (check_unsupported(0) != 0 ||
      check_unsupported(RSEQ_FLAG_UNREGISTER) != 0)
    atomic_fetch_add_explicit(&failures, 1, memory_order_relaxed);
  for (int i = 0; i < INCREMENTS; ++i)
    atomic_fetch_add_explicit(&fallback_count, 1, memory_order_relaxed);
  return NULL;
}

int main(void) {
  pthread_t threads[WORKERS];
  for (int i = 0; i < WORKERS; ++i)
    if (pthread_create(&threads[i], NULL, worker, NULL) != 0) return 1;
  for (int i = 0; i < WORKERS; ++i)
    if (pthread_join(threads[i], NULL) != 0) return 2;
  if (atomic_load(&failures) != 0 ||
      atomic_load(&fallback_count) != WORKERS * INCREMENTS) return 3;
  puts("GUEST_RSEQ_FALLBACK_OK");
  return 0;
}
