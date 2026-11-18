# ARM64 syscall 191: `semctl` host-ID safeguard

Darwin System V semaphores use a host-global numeric ID namespace. Before
this change, Shimmy translated several Linux `semctl` commands and forwarded
any guest-supplied ID to Darwin. `IPC_RMID`, `SETVAL`, and `SETALL` could
delete or change a host semaphore even though guest `semget` is disabled.
Read commands could expose host semaphore state.

`semctl` now returns `ENOSYS` for every command before reading guest memory
or calling host IPC. This is a bounded safeguard, not Linux semaphore
support. `semop` and `semtimedop` still accept raw host IDs and need their
own guards. A real implementation needs a guest-owned semaphore backend or
instance-scoped ID broker shared by all four calls, with permissions,
operation semantics, fork/exec lifetime, timeout handling, and cleanup.

Verification: `./build.sh cli` and `python3 tests/run-semctl-refusal.py`.
The disposable ARM64 guest checks deletion, value reads and writes, array
reads and writes, an invalid ID, and an unknown command. It creates and
accesses no host semaphore. A private Arch boot smoke is a separate gate.
That isolated boot reached a fresh login prompt with D-Bus broker and
launcher settled; it does not establish SysV semaphore compatibility.
