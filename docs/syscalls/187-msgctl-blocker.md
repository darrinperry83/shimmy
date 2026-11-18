# `msgctl` (AArch64 syscall 187): host queue-ID safeguard

Shimmy formerly forwarded guest-supplied numeric queue IDs to Darwin's
host-global System V message queue namespace. `IPC_RMID` could delete a host
queue, `IPC_SET` could change it, and `IPC_STAT` could disclose host queue and
process metadata. Disabling `msgget` alone did not block these ID-taking calls.

`msgctl` now returns `ENOSYS` for every command before reading guest pointers
or calling host `msgctl`. This is a safeguard, not message queue compatibility.
`msgrcv` and `msgsnd` still accept raw host IDs and require their own guards.

A complete implementation needs guest-scoped queue IDs and a shared backend
across `msgget`, `msgctl`, `msgrcv`, and `msgsnd`, with permissions, IPC command
translation, fork/exec lifetime and cleanup. The existing `lx_msqid64_ds`
translation helpers were removed because the guarded syscall had no callers
for them; a future backend must implement and verify ABI translation anew.

Proof: `./build.sh cli` and `python3 tests/run-msgctl-refuse.py`. The ARM64
probe tests `IPC_RMID`, `IPC_SET`, `IPC_STAT`, unknown command and invalid ID,
and verifies its output buffer is unchanged. It creates or accesses no host
queue. A private Arch boot smoke is a separate integration gate.
That isolated boot reached a fresh login prompt with D-Bus broker and
launcher settled; it does not establish message-queue compatibility.
