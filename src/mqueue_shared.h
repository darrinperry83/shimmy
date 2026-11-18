#ifndef SHIMMY_MQUEUE_SHARED_H
#define SHIMMY_MQUEUE_SHARED_H

#include <stddef.h>

// Inactive, guest-private cross-process queue store. dir must be a 0700
// directory owned by the shim user and shared only among one guest session.
// Returned fds are INTERNAL host fds, never guest-visible mqueue descriptors.
int mq_shared_open(const char* dir, const char* name, int create, int exclusive,
                   unsigned max_messages, unsigned message_size, int* out_fd);
int mq_shared_unlink(const char* dir, const char* name);
int mq_shared_send(int fd, const void* data, size_t length, unsigned priority);
int mq_shared_receive(int fd, void* data, size_t capacity,
                      size_t* length, unsigned* priority);
int mq_shared_status(int fd, unsigned* count, unsigned* max_messages,
                     unsigned* message_size, int* readable, int* writable);
// Serialize close with this process's queue operations. A guest fd adapter
// must use this instead of raw close for backing queue descriptors.
int mq_shared_close(int fd);

enum { MQ_SHARED_READABLE = 1, MQ_SHARED_WRITABLE = 2 };
// Backend readiness wait. timeout_ms is relative, or -1 for no deadline.
// This is not a guest poll/epoll adapter; the backing regular file must not
// be passed to host poll as if it were a Linux mqueue descriptor.
int mq_shared_wait_ready(int fd, int wanted, int timeout_ms, int* ready);

#endif
