/* Owned domain metadata; native proc/task/thread offsets stay unchanged.
 * Native IDs remain in use at this checkpoint. This is not PID namespace ABI. */
#include <os/refcnt.h>
struct rd_linux_domain {
    os_refcnt_t references;
    uint64_t identity;
    pid_t leader_pid;
    uint32_t closing;
};
struct rd_linux_domain *rd_linux_domain_hold(struct proc *);
void rd_linux_domain_release(struct rd_linux_domain *);
uint64_t rd_linux_domain_id(struct proc *);
bool rd_linux_domain_same(struct proc *,struct proc *);
bool rd_linux_domain_leader(struct proc *);
bool rd_linux_domain_closing(struct proc *);
bool rd_linux_domain_target(struct proc *,struct proc *);
void rd_linux_domain_exit(struct proc *);
int rd_linux_domain_inherit(struct proc *,struct proc *);
void rd_linux_domain_forget(struct proc *);
