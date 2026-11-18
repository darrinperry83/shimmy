# `kill` (AArch64 syscall 129)

The bounded positive-PID path accepts the current guest PID, including virtual
PID 1, and a known direct guest child. It translates those to the shim's own
host PID or the registered child; unrelated host PIDs are never accepted as
guest targets. Signal zero checks existence without delivery.

Group and broadcast selectors (`0`, `-1`, negative process group IDs) now
return `EOPNOTSUPP`. The former implementation signaled only the shim itself,
which silently violated Linux's group/broadcast contract. A guest process
group registry and scoped signal fanout are still needed. Unsupported signal
numbers and guest signal-handler delivery remain separate limitations.

`python3 tests/run-kill-scope.py` checks ordinary and virtual-PID-1 guests,
self and unrelated targets, refusal before group signal delivery, and signal
zero/SIGKILL delivery to a known child. The test does not prove all Linux
signal permissions, PID namespaces, or group semantics.

Reference: [Linux `kill(2)`](https://man7.org/linux/man-pages/man2/kill.2.html).
