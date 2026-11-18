#ifndef SHIMMY_MQUEUE_FD_REGISTRY_H
#define SHIMMY_MQUEUE_FD_REGISTRY_H

#include "mqueue_core.h"

// Inactive mqueue descriptor model. The caller owns allocation/closing of the
// integer guest fd; this table only records mqueue identity and alias state.
// It is process-local and cannot be used for guest fork/exec until a shared
// queue broker and descriptor reattachment exist.
int mqfd_bind(int fd, struct mq_core_handle* handle, int access_mode,
              int nonblock, int cloexec);
int mqfd_duplicate(int oldfd, int newfd, int cloexec);
int mqfd_detach(int fd);
int mqfd_acquire(int fd, struct mq_core_handle** handle,
                 int* access_mode, int* nonblock, int* cloexec);
int mqfd_set_nonblock(int fd, int nonblock);
int mqfd_set_cloexec(int fd, int cloexec);

#endif
