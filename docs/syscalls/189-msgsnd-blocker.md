# ARM64 #189 `msgsnd`: host queue ID safeguard

Darwin SysV message queue IDs inhabit a host-global namespace. The published
handler passed an arbitrary guest integer ID and raw guest buffer pointer to
host `msgsnd`, which could address another Mac process's queue. Shimmy's
`msgget` already returns `ENOSYS`; it cannot mint a guest-owned queue ID.

This correction refuses every #189 call with `ENOSYS` before touching guest
memory or the host SysV queue API. It provides no message-send behavior.
`python3 tests/run-msgsnd-refused.py` compiles and runs three ARM64 calls,
including a bad pointer and nonzero message size, without creating or
accessing a queue.

Real support needs an instance-scoped queue ID registry, permission checks,
queue lifecycle and coordination across #186–189. The remaining #187 and #188
raw ID-taking handlers are separate unresolved safeguards.
