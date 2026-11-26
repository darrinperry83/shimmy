# Native Linux init domain on XNU

The immediate kernel milestone for full Arch init boot is a coherent Linux init
domain. The current XNU tests retain native process identities; systemd is not
PID 1 and its manager startup guard is still expected to reject that context.
The design below is a required next implementation, not an implemented feature.

## Ownership and identity

Keep XNU's native init/launchd and supporting Darwin services. Create an owned
Linux process domain whose real init process has Linux PID 1 within that domain.
XNU still owns its native proc/task, scheduling, VM and hardware. Linux IDs are
kernel-maintained namespace identities attached to actual processes; they are
not a special getpid result or a userspace wrapper.

The domain needs a process table, init reference, non-reused live IDs and an
explicit lifetime. Add references through the existing owned Linux sidecar;
preserve existing native proc/task/thread field offsets and the KDK entry path.
Fork must publish a child identity before it can run. Exec must transfer domain
identity across native proc replacement while resetting the appropriate exec
state. Failure paths, native process destruction and reap must release references
exactly once. Native Darwin queries keep their native identities.

Every Linux identity surface must agree: getpid/getppid/gettid, clone return,
waitid/wait4, signalfd sender/child metadata, kill selectors, sessions/process
groups and proc entries. Resolve target IDs to held native proc references;
never pass a namespace PID straight to native kill/wait lookup. Preserve native
credentials and permission checks after translation. Linux operations must not
target supporting Darwin services outside the domain.

## Adoption and shutdown

An orphan in the Linux domain must be reparented to the nearest supported Linux
subreaper or its domain init. Preserve native parent/child list locking, wait
ownership, zombie status and SIGCHLD notification. Audit XNU's global-init
auto-reap flags: merely changing the parent pointer does not establish Linux
init waiting behavior. Already-dead children must also remain reapable by the
correct adopter. Do not redirect Darwin orphans into the Linux domain.

Init death must terminate and collect remaining domain members, unblock waiters
and release namespace resources without affecting launchd or Apple services.
The external native supervisor must receive a truthful result and remain able
to start a fresh domain. Existing single-thread fork/exec limitations remain
explicit until a separate thread implementation is qualified.

## First kernel acceptance test

Use the same Linux ELF on reference Linux and the custom XNU kernel to require:

1. A real init domain with consistent init/child/grandchild IDs and parent IDs.
2. Fork/exec identity continuity and correct native-supervisor separation.
3. Orphan adoption, SIGCHLD, exact exit status and one-time reaping.
4. Consistent signal and session/group target translation; reject escape.
5. Init-death cleanup, no leaked children and successful creation of another domain.
6. All previous Linux gates plus native Darwin process/wait controls on that boot.

Only then expose this domain through the privileged native boot launcher and
connect the complete installer root. The eventual launcher must bind one root
and create one domain atomically; it must not advertise a booted systemd runtime
before systemd establishes it. Runtime filesystem views must come from actual
domain/native state. Root mounting, device/broker access, proc/cgroup contracts,
full unit boot and installer operation each require their own runtime evidence.
