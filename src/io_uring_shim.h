// io_uring_shim.h — minimal synchronous io_uring for the Linux aarch64 guest.
//
// The shim has no async I/O engine, so io_uring is emulated as a SYNCHRONOUS
// fallback: every SQE submitted through io_uring_enter is executed inline and a
// CQE is written back before the call returns. Programs get correct results
// (they lose only the async speedup), which is exactly the degradation
// systemd/curl/nginx tolerate.
//
// The rings are real MAP_SHARED host memory backed by an unlinked temp file, so
// the guest's mmap(IORING_OFF_SQ_RING / CQ_RING / SQES) requests resolve to fresh
// aliases that share memory with the shim's canonical view, and a guest munmap
// only releases its own alias. See io_uring_shim.c for the layout.
#ifndef RD_IO_URING_SHIM_H
#define RD_IO_URING_SHIM_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// Linux aarch64 io_uring syscalls. Same ABI as linux_syscall(): >= 0 on success,
// negative LINUX errno on failure.
long ioring_setup(unsigned entries, uint64_t params_guest);
long ioring_enter(unsigned fd, unsigned to_submit, unsigned min_complete,
                  unsigned flags, uint64_t sig_guest, uint64_t sigsz);
long ioring_register(unsigned fd, unsigned opcode, uint64_t arg_guest,
                     unsigned nr_args);

// mmap(2) hook (syscall 222): is `fd` one of our io_uring ring fds?
int  ioring_owns_fd(int fd);
// Map the ring sub-region named by the guest's IORING_OFF_* `off` as a fresh
// MAP_SHARED alias. Returns NULL when the fd/offset does not name one of ours.
void* ioring_mmap(int fd, long off, size_t len);

// fd lifecycle. ioring_fd_closed drops the per-fd registry entry and releases
// the canonical backing only when the last descriptor referencing the ring goes
// away (the actual host fd is closed by the close(2)/close_range(2) caller).
void ioring_fd_closed(int fd);
// A dup/dup2/dup3/F_DUPFD of an io_uring fd keeps the same ring.
void ioring_register_fd_copy(int newfd, int oldfd);

#ifdef __cplusplus
}
#endif
#endif
