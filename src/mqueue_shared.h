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

#endif
