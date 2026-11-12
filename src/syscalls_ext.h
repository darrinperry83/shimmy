// syscalls_ext.h — extended/rarer Linux syscalls (fallback tier for linux_syscall).
#ifndef RDRIFT_SYSCALLS_EXT_H
#define RDRIFT_SYSCALLS_EXT_H
#ifdef __cplusplus
extern "C" {
#endif

void syscalls_ext_set_root(const char* root);

// Same ABI as linux_syscall(): negative LINUX errno on failure.
// Returns -38 (-ENOSYS) for anything genuinely unimplemented.
long linux_syscall_ext(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                       unsigned long a3, unsigned long a4, unsigned long a5);

#ifdef __cplusplus
}
#endif
#endif
