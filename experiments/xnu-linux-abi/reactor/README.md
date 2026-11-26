# Native epoll, timerfd, signalfd and private futex batch

This opt-in `--with-linux-reactor` profile extends the proven native eventfd
backend with the next systemd event-loop interfaces. It preserves the existing
shim's AArch64 layouts and readiness contracts while using native kernel
backends. It introduces no userspace syscall interception or readiness worker.
A successful build/reference test is separate from runtime acceptance.

The batch is now accepted on live custom XNU kernel
`CF899E7A-3A07-3D2B-A660-0C052D0F00D4`. The raw fixture and original Arch
libsystemd callback loop both pass; the latter exits zero with empty stderr.
All earlier regression gates pass on that boot, with WindowServer absent.
See [evidence.json](evidence.json) for hashes, logs and bounded qualification.

## Epoll

Epoll create1/ctl/pwait use a native fileglob and persistent kqueue, with at most
32 registrations/events. IN/OUT filters are coalesced by registration identity,
not by caller userdata; equal userdata on different descriptors remains two
events. Level readiness, ADD/MOD/DEL, 64-bit userdata, copyout-fault retention,
one-shot rearming and fair bounded returns are covered by the reference fixture.
One-shot disables both filters only after that registration is delivered;
unreturned registrations stay armed. EPOLLET and temporary pwait signal masks
return EOPNOTSUPP. Pipe/eventfd/timerfd/signalfd sources are supported; regular
vnode sources return EPERM and unqualified descriptor classes return EOPNOTSUPP.

The Linux-facing epoll fileglob can be duplicated and inherited across fork.
The kqueue subscriptions remain with the creating process. Service children
can close inherited descriptors or exec with CLOEXEC while the parent continues
waiting. Child ctl/wait operations are explicitly unsupported. A native fdclose
hook retires registrations before fd-number reuse; the interest table contains
no dangling fileglob pointer. Closing a watched owner fd while other aliases
still keep its fileglob alive returns EOPNOTSUPP: native kqueue's per-fd close
rules do not yet provide Linux's full open-file-description alias lifetime.
Delete that registration first, or close the other aliases first.

Owner exec/exit cleans up kqueue subscriptions before native fd-table teardown.
A lifetime mutex serializes that cleanup against a last close in a child, so
no process reference is pinned and a retained child fileglob cannot keep a
dangling owner. Operations on that orphaned object return EOPNOTSUPP. Owner
exec with a non-CLOEXEC epoll descriptor is rejected until subscription transfer
across native exec is implemented. These are bounded lifecycle rules, not full
Linux shared-epoll or concurrent-thread qualification. Nested epoll and polling
an epoll descriptor are also explicitly unsupported.

## Timerfd

Timerfd create/settime/gettime use native delayed thread calls and the existing
counter/readiness backend. CLOCK_MONOTONIC and CLOCK_BOOTTIME support relative
and absolute deadlines, one-shot and periodic expiry, overrun counts, disarm,
old/remaining time and shared fileglob lifetime. Boottime scheduling uses XNU's
continuous-time flag; physical suspend/wake remains a separate qualification.
Final close prevents rearming, cancels/waits for the callback and frees its state.
Timer write/OUT readiness are not advertised as counter operations.
CLOCK_REALTIME/alarm clocks and CANCEL_ON_SET remain explicit unsupported modes.

## Signalfd

Signalfd4 reads XNU's blocked native pending-signal bits, consuming the actual
pending signal rather than a second delivery queue. It emits the 128-byte Linux
record, including sender identity and child exit metadata stored in the owned
Linux process sidecar. Standard signals coalesce and are read in Linux signal
number order. The mask can be updated without changing existing descriptor
flags. Supported process-directed signals must be blocked; realtime signals,
unmapped signals and complete signal-generation modes remain unsupported.

A private kqueue filter observes pending readiness. Notification occurs after
native pending state is stored and uses a separate private hint, without
incrementing Darwin's normal EVFILT_SIGNAL counts. Blocking read sleeps against
the native process lock/pending channel, preventing a check/sleep lost wakeup.
Default-ignored signals that Linux has blocked are retained for its signalfd,
while explicit SIG_IGN and native Darwin processes keep native behavior.
Only metadata is added to the existing owned sidecar; native process/thread
field offsets stay unchanged. Fork does not inherit pending metadata.

## Private futex

FUTEX_WAIT_PRIVATE and FUTEX_WAKE_PRIVATE use a kernel-owned waiter list and
XNU's interruptible `msleep`/thread wakeup. The native mutex covers value
comparison, waiter publication and wake selection; msleep asserts the wait
before releasing that same mutex. Keys include the native process identity,
so equal virtual addresses in forked address spaces do not alias. Stack-owned
waiters are removed under the mutex on wake, timeout or interruption.

The raw fixture covers compare mismatch, bad pointers/alignment/timeouts,
empty wake, zero/finite relative timeout, elapsed native sleep, and fork/COW
isolation. Original libsystemd exercises the empty private wake used by libc's
one-time initialization. This single-thread process milestone does not qualify
positive multi-waiter wake races, robust cleanup, shared/PI/requeue/bitset futexes
or complete pthread support. Other futex modes return EOPNOTSUPP; relative
timeouts exceeding INT_MAX seconds are also explicitly unsupported.

## Reference and acceptance

```sh
python3 experiments/xnu-linux-abi/reactor/verify.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-reactor
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-reactor
python3 experiments/xnu-linux-abi/stage_vm_variant.py reactor
python3 experiments/xnu-linux-abi/startup/verify_runtime.py <unique-staged-name>
```

The unchanged raw Linux ELF covers the combined interfaces and forces separate
peer wakeups for blocking epoll and signalfd. Its service child forks with an
open epoll descriptor and execs before signaling the inherited event counter.
The timer controls cover periodic accumulation, absolute boottime, disarm and
repeated immediate-arm/final-close callback races. The signal controls cover
mask updates, coalescing, sender identity, copy faults, and real SIGCHLD with
child status/reaping. XNU-only controls check explicit unsupported modes.

The second test links a small callback application to the unchanged Arch
libsystemd.so.0. The original library runs sd_event_new/add_io/add_signal/add_time
and sd_event_loop, including an SD_EVENT_ONESHOT IO source. The copied Arch
loader/library closure also passes inside a minimal Linux chroot reference.
The XNU gate must run that same executable/library closure directly inside the
native chroot and obtain all three callbacks, zero exit and empty stderr.
This is a library event-loop gate, not systemd PID 1 or a running service manager.

The selected systemd source explicitly adds EPOLLONESHOT for one-shot IO sources:
[sd-event.c v262.2](https://github.com/systemd/systemd/blob/v262.2/src/libsystemd/sd-event/sd-event.c).
Native qnotes, descriptor ownership, timers and pending signals remain
independently auditable in the pinned XNU source overlay. Keep the kernel,
personality and Arch package pins separate. General proc/cgroup/rootfs and init
context, full signal/thread support, networking, shell/input and physical-driver
qualification are not supplied by this batch.
