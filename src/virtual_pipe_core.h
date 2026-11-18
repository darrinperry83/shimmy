#ifndef SHIMMY_VIRTUAL_PIPE_CORE_H
#define SHIMMY_VIRTUAL_PIPE_CORE_H
#include <stddef.h>

/* Inactive pipe-buffer primitive. It does not represent guest file descriptors
 * or participate in poll/epoll yet. Use synthetic test bytes only. */
typedef struct vp_core vp_core;
enum { VP_CAPACITY = 4096 };
vp_core *vp_core_create(const char *private_directory, char *path, size_t path_capacity);
vp_core *vp_core_open(const char *path);
void vp_core_close(vp_core *pipe);
long vp_core_write(vp_core *pipe, const void *bytes, size_t length);
long vp_core_read(vp_core *pipe, void *bytes, size_t length);
long vp_core_tee(vp_core *source, vp_core *destination, size_t length);
long vp_core_pending(vp_core *pipe);
long vp_core_space(vp_core *pipe);
#endif
