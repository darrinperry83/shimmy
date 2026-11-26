/* Private sidecar. Existing proc/task/thread field offsets are retained. */
#include <kern/locks.h>
struct rd_linux_proc_state {
    lck_mtx_t heap_lock;
    uint64_t heap_base, heap_current;
};
void rd_linux_state_fork(struct proc *, struct proc *);
void rd_linux_state_free(struct proc *);
void rd_linux_machine_exec(thread_t, bool, uint64_t);
void rd_linux_machine_set_clear_tid(thread_t, uint64_t);
uint64_t rd_linux_machine_clear_tid(thread_t);
void rd_linux_machine_set_mask(thread_t, uint64_t);
uint64_t rd_linux_machine_mask(thread_t);
