# `mq_open` (ARM64 syscall 180): blocked by POSIX message-queue backend

Linux `mq_open(name, oflag, mode, attr)` opens or creates a named queue and returns a descriptor. That descriptor is useful to `mq_timedsend`, `mq_timedreceive`, `mq_getsetattr`, `mq_notify`, `poll`, `close`, `dup`, and inheritance across `fork`/`exec`; queue names persist until `mq_unlink`. Creation also applies mode, message-size/count limits, and per-user queue accounting. The queue orders messages by priority and supports blocking and absolute-timeout operations.

Shimmy currently sends ARM64 syscalls 180–185 to `ENOSYS`. Its Darwin-host SysV message-queue forwarding at 186–191 is a different API: a SysV queue ID cannot serve as a POSIX queue descriptor or reproduce POSIX naming, readiness, notification, and lifecycle. Darwin provides no POSIX mqueue syscall backend in this implementation. Returning a synthetic descriptor from `mq_open` while send, receive, notify, and attribute calls still fail would falsely indicate a usable queue.

Keep the current `ENOSYS` result. To implement this family, first add a guest-owned queue registry and descriptor type with shared open-file state, priority-ordered messages, limits/accounting, blocking and timeout behavior, poll readiness, notification ownership, and unlink/refcount lifecycle. Then test `mq_open` together with send, receive, attributes, notify, unlink, `poll`, `dup`, `fork`, `exec`, and `close` in a disposable ARM64 guest root. Compare error and race cases against Linux before enabling syscall 180.

Primary source: [Linux `ipc/mqueue.c`](https://github.com/torvalds/linux/blob/master/ipc/mqueue.c), especially the `mq_open` syscall and mqueue file operations.
