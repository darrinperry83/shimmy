#include "mqueue_core.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// This module deliberately has no host-global name, file, or socket side
// effect. A later fd adapter must provide interprocess state and readiness.
enum { MQ_CORE_MAX_QUEUES = 64, MQ_CORE_MAX_NAME = 255,
       MQ_CORE_MAX_MESSAGES = 1024, MQ_CORE_MAX_MSG_SIZE = 65536,
       MQ_CORE_MAX_PRIORITY = 32767 };

struct mq_core_message {
  struct mq_core_message* next;
  size_t length;
  unsigned priority;
  unsigned char data[];
};

struct mq_core_queue {
  struct mq_core_queue* next;
  char name[MQ_CORE_MAX_NAME + 1];
  size_t max_messages, message_size, count, refs;
  int linked;
  pthread_cond_t not_empty, not_full;
  struct mq_core_message* head;
};

struct mq_core_handle { struct mq_core_queue* queue; };

static pthread_mutex_t g_mq_mu = PTHREAD_MUTEX_INITIALIZER;
static struct mq_core_queue* g_mq_queues;

static void mq_free_message(struct mq_core_message* msg) {
  // A volatile store prevents the compiler from eliding payload erasure.
  volatile unsigned char* data = msg->data;
  for (size_t i = 0; i < msg->length; ++i) data[i] = 0;
  free(msg);
}

static int mq_name_valid(const char* name) {
  if (!name || name[0] != '/' || !name[1]) return 0;
  size_t n = 0;
  for (const char* p = name; *p; ++p) {
    if (++n > MQ_CORE_MAX_NAME || (p != name && *p == '/')) return 0;
  }
  return 1;
}

static struct mq_core_queue* mq_find_locked(const char* name) {
  for (struct mq_core_queue* q = g_mq_queues; q; q = q->next)
    if (q->linked && strcmp(q->name, name) == 0) return q;
  return NULL;
}

static void mq_destroy_locked(struct mq_core_queue* queue) {
  struct mq_core_queue** link = &g_mq_queues;
  while (*link && *link != queue) link = &(*link)->next;
  if (*link == queue) *link = queue->next;
  struct mq_core_message* msg = queue->head;
  while (msg) {
    struct mq_core_message* next = msg->next;
    mq_free_message(msg);
    msg = next;
  }
  pthread_cond_destroy(&queue->not_empty);
  pthread_cond_destroy(&queue->not_full);
  free(queue);
}

int mq_core_open(const char* name, int create, int exclusive,
                 size_t max_messages, size_t message_size,
                 struct mq_core_handle** out) {
  if (!out) return EFAULT;
  *out = NULL;
  if (!mq_name_valid(name)) return EINVAL;
  pthread_mutex_lock(&g_mq_mu);
  struct mq_core_queue* queue = mq_find_locked(name);
  int created = 0;
  if (queue && create && exclusive) { pthread_mutex_unlock(&g_mq_mu); return EEXIST; }
  if (!queue && !create) { pthread_mutex_unlock(&g_mq_mu); return ENOENT; }
  if (!queue) {
    if (!max_messages) max_messages = 10;
    if (!message_size) message_size = 8192;
    if (max_messages > MQ_CORE_MAX_MESSAGES || message_size > MQ_CORE_MAX_MSG_SIZE ||
        message_size > SIZE_MAX / max_messages) {
      pthread_mutex_unlock(&g_mq_mu);
      return EINVAL;
    }
    size_t total = 0;
    for (struct mq_core_queue* q = g_mq_queues; q; q = q->next) ++total;
    if (total >= MQ_CORE_MAX_QUEUES) { pthread_mutex_unlock(&g_mq_mu); return ENOSPC; }
    queue = calloc(1, sizeof *queue);
    if (!queue) { pthread_mutex_unlock(&g_mq_mu); return ENOMEM; }
    int err = pthread_cond_init(&queue->not_empty, NULL);
    if (err) { free(queue); pthread_mutex_unlock(&g_mq_mu); return err; }
    err = pthread_cond_init(&queue->not_full, NULL);
    if (err) {
      pthread_cond_destroy(&queue->not_empty);
      free(queue);
      pthread_mutex_unlock(&g_mq_mu);
      return err;
    }
    memcpy(queue->name, name, strlen(name) + 1);
    queue->max_messages = max_messages;
    queue->message_size = message_size;
    queue->linked = 1;
    queue->next = g_mq_queues;
    g_mq_queues = queue;
    created = 1;
  }
  struct mq_core_handle* handle = malloc(sizeof *handle);
  if (!handle) {
    if (created) mq_destroy_locked(queue);
    pthread_mutex_unlock(&g_mq_mu);
    return ENOMEM;
  }
  ++queue->refs;
  handle->queue = queue;
  *out = handle;
  pthread_mutex_unlock(&g_mq_mu);
  return 0;
}

int mq_core_unlink(const char* name) {
  if (!mq_name_valid(name)) return EINVAL;
  pthread_mutex_lock(&g_mq_mu);
  struct mq_core_queue* queue = mq_find_locked(name);
  if (!queue) { pthread_mutex_unlock(&g_mq_mu); return ENOENT; }
  queue->linked = 0;
  if (!queue->refs) mq_destroy_locked(queue);
  pthread_mutex_unlock(&g_mq_mu);
  return 0;
}

void mq_core_close(struct mq_core_handle* handle) {
  if (!handle) return;
  pthread_mutex_lock(&g_mq_mu);
  struct mq_core_queue* queue = handle->queue;
  if (--queue->refs == 0 && !queue->linked) mq_destroy_locked(queue);
  free(handle);
  pthread_mutex_unlock(&g_mq_mu);
}

static int mq_deadline_valid(const struct timespec* deadline) {
  return !deadline ||
      (deadline->tv_sec >= 0 && deadline->tv_nsec >= 0 && deadline->tv_nsec < 1000000000L);
}

static int mq_send(struct mq_core_handle* handle, const void* data,
                   size_t length, unsigned priority, int wait,
                   const struct timespec* deadline) {
  if (!handle || (length && !data)) return EFAULT;
  if (priority > MQ_CORE_MAX_PRIORITY) return EINVAL;
  if (!mq_deadline_valid(deadline)) return EINVAL;
  pthread_mutex_lock(&g_mq_mu);
  struct mq_core_queue* queue = handle->queue;
  if (length > queue->message_size) { pthread_mutex_unlock(&g_mq_mu); return EMSGSIZE; }
  while (queue->count == queue->max_messages) {
    if (!wait) { pthread_mutex_unlock(&g_mq_mu); return EAGAIN; }
    int err = deadline
        ? pthread_cond_timedwait(&queue->not_full, &g_mq_mu, deadline)
        : pthread_cond_wait(&queue->not_full, &g_mq_mu);
    if (err) { pthread_mutex_unlock(&g_mq_mu); return err; }
  }
  struct mq_core_message* msg = malloc(sizeof *msg + length);
  if (!msg) { pthread_mutex_unlock(&g_mq_mu); return ENOMEM; }
  msg->next = NULL;
  msg->length = length;
  msg->priority = priority;
  if (length) memcpy(msg->data, data, length);
  // Descending priority, FIFO among equal priorities.
  struct mq_core_message** link = &queue->head;
  while (*link && (*link)->priority >= priority) link = &(*link)->next;
  msg->next = *link;
  *link = msg;
  ++queue->count;
  pthread_cond_signal(&queue->not_empty);
  pthread_mutex_unlock(&g_mq_mu);
  return 0;
}

int mq_core_send(struct mq_core_handle* handle, const void* data,
                 size_t length, unsigned priority) {
  return mq_send(handle, data, length, priority, 0, NULL);
}

int mq_core_timedsend(struct mq_core_handle* handle, const void* data,
                      size_t length, unsigned priority,
                      const struct timespec* abs_realtime) {
  return mq_send(handle, data, length, priority, 1, abs_realtime);
}

static int mq_receive(struct mq_core_handle* handle, void* data,
                      size_t capacity, size_t* length, unsigned* priority,
                      int wait, const struct timespec* deadline) {
  if (!handle || !length || (capacity && !data)) return EFAULT;
  if (!mq_deadline_valid(deadline)) return EINVAL;
  pthread_mutex_lock(&g_mq_mu);
  struct mq_core_queue* queue = handle->queue;
  if (capacity < queue->message_size) { pthread_mutex_unlock(&g_mq_mu); return EMSGSIZE; }
  while (!queue->head) {
    if (!wait) { pthread_mutex_unlock(&g_mq_mu); return EAGAIN; }
    int err = deadline
        ? pthread_cond_timedwait(&queue->not_empty, &g_mq_mu, deadline)
        : pthread_cond_wait(&queue->not_empty, &g_mq_mu);
    if (err) { pthread_mutex_unlock(&g_mq_mu); return err; }
  }
  struct mq_core_message* msg = queue->head;
  queue->head = msg->next;
  --queue->count;
  if (msg->length) memcpy(data, msg->data, msg->length);
  *length = msg->length;
  if (priority) *priority = msg->priority;
  mq_free_message(msg);
  pthread_cond_signal(&queue->not_full);
  pthread_mutex_unlock(&g_mq_mu);
  return 0;
}

int mq_core_receive(struct mq_core_handle* handle, void* data,
                    size_t capacity, size_t* length, unsigned* priority) {
  return mq_receive(handle, data, capacity, length, priority, 0, NULL);
}

int mq_core_timedreceive(struct mq_core_handle* handle, void* data,
                         size_t capacity, size_t* length, unsigned* priority,
                         const struct timespec* abs_realtime) {
  return mq_receive(handle, data, capacity, length, priority, 1, abs_realtime);
}

int mq_core_status(struct mq_core_handle* handle, size_t* count,
                   size_t* max_messages, size_t* message_size,
                   int* readable, int* writable) {
  if (!handle) return EFAULT;
  pthread_mutex_lock(&g_mq_mu);
  struct mq_core_queue* queue = handle->queue;
  if (count) *count = queue->count;
  if (max_messages) *max_messages = queue->max_messages;
  if (message_size) *message_size = queue->message_size;
  if (readable) *readable = queue->count != 0;
  if (writable) *writable = queue->count < queue->max_messages;
  pthread_mutex_unlock(&g_mq_mu);
  return 0;
}
