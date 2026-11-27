# Native Linux init domain on XNU

The immediate kernel milestone for full Arch init boot is a coherent Linux init
domain. The current XNU tests retain native process identities; systemd is not
PID 1 and its manager startup guard is still expected to reject that context.
Owned membership/lifetime now has native proof. Linux PID namespace identity and
full systemd init boot remain unimplemented.

The first isolated implementation step is `--with-linux-subreaper`, layered on
the launch checkpoint. It adds Linux `PR_SET_CHILD_SUBREAPER` and
`PR_GET_CHILD_SUBREAPER` to the existing owned sidecar and uses native child lists,
`proc_reparentlocked`, SIGCHLD and wait/reap machinery. Both live and zombie
orphans are eligible; traced children retain the native tracing path. Adoption
stops at a non-Linux ancestor. The property resets on ordinary fork and survives
successful Linux exec, including native proc replacement. This is not yet an init
domain or a full-boot qualification. The domain profile below additionally
matches explicit ownership and falls back to its leader; the subreaper-only
profile retains contiguous Linux ancestry for the earlier regression gate.

The subreaper-only native gate passes on kernel
`ABCA1930-9D5A-3543-84AD-2A7821C0F51D`, staged as
`subreaper-abca1930-e90b1a79`. All earlier regression gates also pass, including
the unchanged Arch libsystemd callback application and systemd --version with
empty stderr. [init/evidence.json](init/evidence.json) pins the source, fixture,
kernel, collection and proof logs. Reproduce the isolated build/stage with:

```sh
python3 experiments/xnu-linux-abi/init/verify.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-subreaper
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-subreaper
python3 experiments/xnu-linux-abi/stage_vm_variant.py subreaper
```

`init/probe.c` is a freestanding ELF shared by the Linux reference and native
tests. Its full mode requires actual PID 1, coherent parent/thread/session IDs,
exec continuity, waitid/WNOWAIT and wait4, live and zombie orphan adoption,
subreapers, and signalfd sender/child metadata. Its `subreaper-only` mode tests the
first kernel step without claiming PID 1. `init/run.c` keeps a native supervisor
outside that lineage, drops its Linux child to uid/gid 65534, and checks native
wait behavior afterward. Run `python3 init/verify.py --linux-reference` from this
directory to build the fixture and run the pinned Linux controls. The init-death
reference only establishes namespace termination in Linux; native teardown,
leak-free collection and independent domains still require their own proof.

## Ownership and identity

The owned-domain gate passes on kernel `9D058C98-E237-3F12-9ACF-3E18D6604B37`,
staged as `domain-9d058c98-17bd994b`. It verifies two fresh admissions/restarts,
actual same-UID external-domain signal rejection, fork/exec continuity, live and
zombie adoption, the existing subreaper ELF inside the domain, leader-death
termination with no retained domain object, and native supervisor controls.
All prior regression gates pass on that boot. Source, fixture, kernel,
collection and log pins are in [init/domain-evidence.json](init/domain-evidence.json).

```sh
python3 experiments/xnu-linux-abi/init/verify_domain.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-domain
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-domain
python3 experiments/xnu-linux-abi/stage_vm_variant.py domain
```

The isolated `--with-linux-domain` implementation adds reference-counted domain
objects referenced by a membership index keyed by XNU process unique ID. XNU
frees `pstats` before publishing a zombie, so the existing heap/signal sidecar
cannot own membership through wait/reap. The index stores only unique IDs and
domain references; parentage, zombie status and wait ownership remain native.
Its buckets do not impose a process-count limit. The diagnostic native
supervisor admits a single-thread process through `kern.rdxnu_linux_domain`
before executing an ELF; writes require native effective root and the opt-in ELF
boot argument. Read-only `kern.rdxnu_linux_domains_live` supports lifetime tests.
These are development interfaces, not the installer boot launcher.

Membership inherits before fork children can run and survives ELF exec and
native proc replacement. The leader is identified by its native PID and XNU
unique ID; no permanent proc reference can block exit. Orphan adoption stops at
the domain boundary and falls back to its leader when no nearer subreaper is
eligible. Leader exit marks the domain closing, uses XNU's process iterator and
temporary proc references to kill remaining members, and leaves native init to
collect terminal orphans. Fork entry rejects closing domains, and a pre-runnable
child check covers creation overlapping the exit scan. Domain references are
released at actual native reap and fork-construction failure. Shadow exec procs
share their logical process's unique ID and do not create or destroy bindings;
failed exec therefore leaves the original membership intact.

This checkpoint retains native process IDs. The domain leader reports parent 0;
it does not report Linux PID 1. Positive-PID signal targeting checks membership
while holding the native proc reference. Domain session/group/prlimit selectors
reject outside targets; other-member queries remain explicitly unsupported
until their dispatch holds references through the complete operation. Darwin
exec from an admitted domain is rejected before image activation. This is not a
complete namespace, security boundary, proc view, or multithread implementation.

`init/domain_probe.c` runs the same lifecycle ELF on reference Linux and XNU.
It reuses the existing subreaper ELF inside its lineage. On XNU,
`init/domain_run.c` also maintains a real same-UID process in another domain,
checks admission privilege/reentry, crosses exec, waits for both live and zombie
orphan adoption, exits with a live descendant, and requires domain-object counts
to return to baseline before starting a fresh domain. The Linux reference uses
an unavailable PID instead of that live external target and does not qualify
native admission/counting. Native runtime qualification is recorded separately.

Keep XNU's native init/launchd and supporting Darwin services. Create an owned
Linux process domain whose real init process has Linux PID 1 within that domain.
XNU still owns its native proc/task, scheduling, VM and hardware. Linux IDs are
kernel-maintained namespace identities attached to actual processes; they are
not a special getpid result or a userspace wrapper.

The domain needs process lookup, init identity, non-reused live IDs and an
explicit lifetime. Reuse XNU's process table with membership checks and held
references rather than duplicating its task/zombie bookkeeping. Add domain
references through the stable-identity membership index; retain per-execution
heap/signal scratch in the existing owned Linux sidecar;
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
