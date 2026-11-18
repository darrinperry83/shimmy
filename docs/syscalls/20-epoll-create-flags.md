# ARM64 #20 `epoll_create1`: flags and close-on-exec

The effective `net.c` route created a Darwin kqueue for every flags value,
then only used the `EPOLL_CLOEXEC` bit. Linux accepts only zero or
`EPOLL_CLOEXEC`; other bits must return `EINVAL` (see the [Linux
manual](https://man7.org/linux/man-pages/man2/epoll_create.2.html)). The
handler now validates flags before allocating a host descriptor.

The raw AArch64 probe also exposed that Darwin `kqueue()` starts with
`FD_CLOEXEC` set in this environment. The existing helper set that bit when
requested but never cleared it for a plain call. The handler now sets or
clears `FD_CLOEXEC` according to the guest flags before registering the new
epoll instance. If host flag setup fails, it closes that new kqueue and
returns the host error.

`./build.sh cli`, `python3 tests/run-epoll-create-flags.py`, the existing
`run-epoll-pwait.py` regression, and an isolated private Arch boot all pass.
The raw probe checks three invalid flags, valid plain and CLOEXEC descriptors,
and close. The Arch gate reached a fresh LOGIN prompt with settled D-Bus;
the exact guest and disposable rootfs clone were cleaned. The baseline probe
failed on its first invalid flag. This change does not establish complete
Linux epoll readiness, wakeup, or nested-instance behavior.
