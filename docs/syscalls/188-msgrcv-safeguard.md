# `msgrcv` (AArch64 syscall 188): host queue ID safeguard

The previous handler passed the guest's numeric `msqid` directly to Darwin
`msgrcv`. Darwin SysV message queues are shared with unrelated macOS processes.
An arbitrary guest ID could therefore read a host queue, block on it, or
consume a message. A guest pointer was also passed without a guest-memory
check. Linux and Darwin sharing the message-buffer layout does not establish
that either the queue ID or memory access is safe.

The bounded safeguard returns `ENOSYS` before reading the queue or buffer.
The ARM64 guest probe covers plausible IDs, blocking and `IPC_NOWAIT` flags,
and an invalid guest buffer; it does not create or access any host queue.

This is **not** Linux-compatible SysV messaging. A real implementation needs
one instance-scoped queue registry/broker for `msgget`, `msgctl`, `msgrcv`, and
`msgsnd`, with guest ID/permission translation, blocking semantics, checked
guest buffers, and queue lifetime cleanup. In particular, syscall 189 remains
a separate raw host queue ID path until safeguarded.
