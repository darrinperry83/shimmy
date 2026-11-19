// syscalls_ext.h — extended/rarer Linux syscalls (fallback tier for linux_syscall).
#ifndef RDRIFT_SYSCALLS_EXT_H
#define RDRIFT_SYSCALLS_EXT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void syscalls_ext_set_root(const char* root);

// Drop a new mount API (fsopen/fsmount) fstype registry entry for `fd` on close(2).
void mountapi_fd_closed(int fd);

// cgroup v2 (/sys/fs/cgroup) synthesis lives in syscalls.c; do_statx needs these to fake the
// statx of the in-memory cgroup dirs/files. cgroup_path_kind: 0 not-cgroup, 1 dir, 2 leaf file.
int cgroup_path_kind(const char* g);
const char* cgroup_file_content(const char* g, char* buf, size_t cap);
// Stable path-derived 64-bit ID for a synthesized cgroup dir/file (see cgroup_handle_id).
uint64_t cgroup_handle_id(const char* g);

// Same ABI as linux_syscall(): negative LINUX errno on failure.
// Returns -38 (-ENOSYS) for anything genuinely unimplemented.
long linux_syscall_ext(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                       unsigned long a3, unsigned long a4, unsigned long a5);

#ifdef __cplusplus
}
#endif
#endif
