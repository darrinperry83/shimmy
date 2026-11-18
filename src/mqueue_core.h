#ifndef SHIMMY_MQUEUE_CORE_H
#define SHIMMY_MQUEUE_CORE_H

#include <stddef.h>
#include <time.h>

// Guest-private POSIX message-queue model. This is not yet an fd or syscall
// adapter; callers must serialize close against use of the same handle.
struct mq_core_handle;

int mq_core_open(const char* name, int create, int exclusive,
                 size_t max_messages, size_t message_size,
                 struct mq_core_handle** out);
int mq_core_unlink(const char* name);
void mq_core_close(struct mq_core_handle* handle);
int mq_core_send(struct mq_core_handle* handle, const void* data,
                 size_t length, unsigned priority);
int mq_core_receive(struct mq_core_handle* handle, void* data,
                    size_t capacity, size_t* length, unsigned* priority);
// Blocking variants use an absolute CLOCK_REALTIME deadline, as Linux's
// mq_timedsend/mq_timedreceive do. NULL means wait without a deadline.
int mq_core_timedsend(struct mq_core_handle* handle, const void* data,
                      size_t length, unsigned priority,
                      const struct timespec* abs_realtime);
int mq_core_timedreceive(struct mq_core_handle* handle, void* data,
                         size_t capacity, size_t* length, unsigned* priority,
                         const struct timespec* abs_realtime);
int mq_core_status(struct mq_core_handle* handle, size_t* count,
                   size_t* max_messages, size_t* message_size,
                   int* readable, int* writable);

#endif
