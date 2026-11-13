// guestthread.h — the little that the syscall layer needs to know about guest threads.
//
// clone() itself lives in shim.c, because creating a thread means reproducing the FULL register
// file at the trap and only the exception handler has it. What leaks out here is just the
// identity questions a syscall has to be able to ask.
#ifndef RDRIFT_GUESTTHREAD_H
#define RDRIFT_GUESTTHREAD_H
#ifdef __cplusplus
extern "C" {
#endif

// Guest tid of the thread currently in a syscall (1 for the initial thread).
int  guest_tid(void);
int  guest_tid_known(int tid);

// HOST child processes the shim created on the guest's behalf (fork, and the posix_spawn behind
// vfork/execve). These are the only pids outside this process that a guest may legitimately name:
// it learned them from fork()/wait4(), which return real host pids. Everything else a guest passes
// to kill() is a number from a namespace it invented, and must not reach the host — that is what
// turned musl's abort() into a signal aimed at launchd before 27d9cc0.
void guest_child_register(int pid);
int  guest_child_known(int pid);

// Non-zero if that thread was created by clone(), i.e. SYS_exit must end the THREAD and not the
// process. The initial thread answers 0, so exit() there still ends the program.
int  guest_is_child(void);

// Publish CLONE_CHILD_CLEARTID and wake any joiner. Call immediately before ending the thread.
void guest_thread_exiting(void);

// set_tid_address(): remember where to write 0 when this thread exits.
void guest_set_tid_address(unsigned long addr);

// Is the calling thread a CLONE_VFORK child? Such a thread must NOT re-exec the shim on
// execve() — that would replace the shared parent process too — and must not end the process on
// exit(). It spawns a host process (or records a status) and releases the blocked parent instead.
int  guest_is_vfork_child(void);

// Called from execve()/exit() on a vfork child. `host_pid` is the process that was spawned, or
// -1 when the child exited without exec'ing (then `status` is its exit code). Never returns.
void guest_vfork_finish(long host_pid, int status);

#ifdef __cplusplus
}
#endif
#endif
