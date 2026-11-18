# `epoll_pwait2` (#441): checked timeout input

The network dispatcher implements a no-signal-mask `epoll_pwait2` wait but
read its optional Linux timespec directly from a guest pointer. The handler
now copies the 16-byte ARM64 timespec into host storage before validation
or waiting. An invalid address returns `EFAULT` without consuming a ready
event. Negative seconds and out-of-range nanoseconds still return `EINVAL`;
a valid zero timeout still reports a ready event.

`./build.sh cli`, `tests/run-epoll-pwait2-timeout.py` with a disposable
no-libc ARM64 guest, and the existing `run-epoll-pwait.py` regression pass.
A separate private Arch clone reached a fresh login prompt with D-Bus
broker and launcher settled; the harness stopped its exact guest and the
clone was removed. A supplied temporary signal mask remains
unsupported because the virtual signal mask and host kqueue wait are not an
atomic operation. Event output staging and broader epoll conformance are
separate work.
