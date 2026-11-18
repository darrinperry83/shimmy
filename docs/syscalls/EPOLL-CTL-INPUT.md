# `epoll_ctl` (#21): checked event input

ADD and MOD previously dereferenced the guest's Linux event pointer
directly in the host process. The network dispatcher now copies the
16-byte event into private storage before changing its interest list or
the host kqueue. An invalid event address returns `EFAULT` without adding
or modifying the watch. DEL continues to ignore its event argument.

`./build.sh cli`, `tests/run-epoll-ctl-input.py` with a disposable no-libc
ARM64 guest, and the existing `run-epoll-pwait.py` regression pass. The
new probe checks invalid ADD/MOD inputs, no ADD side effect, valid data
updates and readiness, and DEL with an invalid event argument. A separate
private Arch clone reached a fresh login prompt with D-Bus broker and
launcher settled; the harness stopped its exact guest and the clone was
removed. This fixes the input boundary, not full Linux epoll conformance.
