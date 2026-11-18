# `msgget` (AArch64 syscall 186): host IPC namespace blocker

The current `msgget` handler forwards the guest's key and flags directly to
Darwin `msgget` (`src/syscalls_ext.c`). Darwin SysV message queues are shared
with other macOS processes under the host account. A guest can therefore
resolve an existing host queue by key or create a queue visible outside its
Shimmy instance. The returned number is a host queue ID, not a guest-private
identifier.

This cannot be corrected safely inside syscall 186 alone. The accompanying
`msgctl`, `msgrcv`, and `msgsnd` handlers (187–189) accept arbitrary numeric
host IDs, so simply translating keys or restricting `msgget` would leave a
path to other queues. `msgctl(IPC_RMID)` could delete one; `IPC_SET` could
alter it; `IPC_STAT` currently reports host owner IDs and sender/receiver
PIDs. A queue created by `IPC_PRIVATE` also needs a lifecycle policy so it
does not leak a persistent host IPC object after a guest exits.

The implementation gate is a shared guest queue registry/broker scoped to a
Shimmy instance. All four calls must resolve guest IDs through that registry,
enforce guest ownership and permissions, translate or suppress host identity
fields, implement key lookup and `IPC_PRIVATE`, and clean up queues at the
right lifecycle boundary. The registry must survive guest fork/exec without
letting another instance claim its IDs. A fully guest-private queue backend
would also avoid exposing host SysV objects outside the instance.

Tests should create two isolated Shimmy instances and an unrelated host
queue, then verify key and ID collision handling, `IPC_PRIVATE`, send/receive
ordering and message types, `IPC_NOWAIT` and blocking wakeups, `IPC_STAT/SET/
RMID`, permissions, fork/exec sharing, and cleanup after abnormal exit. Use
only disposable queues and remove them even when a test fails.

**Status:** security and semantic blocker; this branch makes no runtime
change and does not count syscall 186 as Linux-compatible. No host IPC queue
was created or removed during this audit.
