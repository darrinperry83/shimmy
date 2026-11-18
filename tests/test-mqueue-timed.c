#include "mqueue_core.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct action {
  struct mq_core_handle* handle;
  volatile int started;
  int result;
  char data[2];
  size_t length;
};

static struct timespec deadline_after(int seconds) {
  struct timespec t;
  assert(clock_gettime(CLOCK_REALTIME, &t) == 0);
  t.tv_sec += seconds;
  return t;
}

static void* receiver(void* opaque) {
  struct action* a = opaque;
  struct timespec deadline = deadline_after(1);
  __atomic_store_n(&a->started, 1, __ATOMIC_RELEASE);
  a->result = mq_core_timedreceive(a->handle, a->data, sizeof a->data,
                                    &a->length, NULL, &deadline);
  return NULL;
}

static void* sender(void* opaque) {
  struct action* a = opaque;
  struct timespec deadline = deadline_after(1);
  __atomic_store_n(&a->started, 1, __ATOMIC_RELEASE);
  a->result = mq_core_timedsend(a->handle, "d", 1, 3, &deadline);
  return NULL;
}

static void wait_started(struct action* a) {
  for (int i = 0; i < 1000 && !__atomic_load_n(&a->started, __ATOMIC_ACQUIRE); ++i)
    usleep(1000);
  assert(__atomic_load_n(&a->started, __ATOMIC_ACQUIRE));
  usleep(10000); // allow the helper to reach the blocking condition
}

int main(void) {
  struct mq_core_handle* q = NULL;
  assert(mq_core_open("/shimmy-mq-timed-test", 1, 1, 1, 2, &q) == 0);
  struct timespec past = { .tv_sec = 1, .tv_nsec = 0 };
  struct timespec invalid = { .tv_sec = 1, .tv_nsec = 1000000000L };
  char data[2] = {0};
  size_t length = 0;
  assert(mq_core_timedreceive(q, data, sizeof data, &length, NULL, &past) == ETIMEDOUT);
  assert(mq_core_timedreceive(q, data, sizeof data, &length, NULL, &invalid) == EINVAL);
  assert(mq_core_send(q, "a", 1, 1) == 0);
  assert(mq_core_timedsend(q, "b", 1, 1, &past) == ETIMEDOUT);
  assert(mq_core_receive(q, data, sizeof data, &length, NULL) == 0);
  assert(length == 1 && data[0] == 'a');

  struct action recv_action = { .handle = q };
  pthread_t thread;
  assert(pthread_create(&thread, NULL, receiver, &recv_action) == 0);
  wait_started(&recv_action);
  assert(mq_core_send(q, "b", 1, 2) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(recv_action.result == 0 && recv_action.length == 1 && recv_action.data[0] == 'b');

  assert(mq_core_send(q, "c", 1, 2) == 0);
  struct action send_action = { .handle = q };
  assert(pthread_create(&thread, NULL, sender, &send_action) == 0);
  wait_started(&send_action);
  assert(mq_core_receive(q, data, sizeof data, &length, NULL) == 0);
  assert(data[0] == 'c');
  assert(pthread_join(thread, NULL) == 0);
  assert(send_action.result == 0);
  assert(mq_core_receive(q, data, sizeof data, &length, NULL) == 0);
  assert(data[0] == 'd');

  assert(mq_core_unlink("/shimmy-mq-timed-test") == 0);
  mq_core_close(q);
  puts("mqueue timed core PASS");
  return 0;
}
