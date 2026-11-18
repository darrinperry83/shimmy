# ARM64 #19 `eventfd2`: bounded flag admission

The effective `net.c` handler used a self-connected Darwin datagram socket
and accepted every guest flag value. Unknown bits were ignored; setting
`EFD_SEMAPHORE` returned success even though the socket reads do not implement
Linux's one-at-a-time counter decrement. The [Linux eventfd
manual](https://man7.org/linux/man-pages/man2/eventfd.2.html) defines these
flags and semaphore read behavior.

This change rejects unknown bits with `EINVAL`, then refuses
`EFD_SEMAPHORE` with `EOPNOTSUPP`. Both checks run before the private path,
AF_UNIX socket, or descriptor is created. Existing zero, `EFD_NONBLOCK`,
and `EFD_CLOEXEC` paths are unchanged.

A disposable raw AArch64 probe checks unknown bits alone and combined with a
known bit, semaphore mode alone and with NONBLOCK, and valid zero and
NONBLOCK|CLOEXEC descriptor flags. The baseline fails the first invalid flag;
the candidate passes. `./build.sh cli` and an isolated private Arch boot pass;
the boot reached a fresh LOGIN prompt with D-Bus settled, then the harness
stopped its exact guest and the disposable rootfs clone was removed. The
valid AF_UNIX-backed probe needed an unsandboxed test run because the default
sandbox denied socket creation.

This is a flag safeguard, not full eventfd compatibility. The existing
datagram backend does not provide Linux counter aggregation, saturation,
short-read errors, or all fork/dup/poll behavior. A guest counter model and
shared descriptor lifecycle are separate work.
