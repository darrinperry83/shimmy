# `epoll_pwait` (#22): checked event output

The shared epoll wait helper wrote Linux event records directly through the
guest output pointer. It now validates the bounded declared capacity before
waiting, stages up to 4096 event records in host-owned memory, and copies
only returned events through checked guest memory. An invalid destination
returns `EFAULT` before consuming a ready event. This helper also serves
`epoll_pwait2` (#441), so its no-mask output receives the same boundary.

`./build.sh cli`, `tests/run-epoll-pwait.py`, and
`tests/run-epoll-pwait2-timeout.py` pass. Both disposable no-libc ARM64
probes verify that an invalid output pointer leaves a ready event for a
subsequent valid wait. Supplied temporary signal masks remain unsupported
because mask installation and kqueue waiting are not atomic. The bounded
capacity preflight may reject a destination smaller than `maxevents` even
when fewer events are ready. A separate private Arch clone reached a fresh
login prompt with D-Bus broker and launcher settled; the harness stopped its
exact guest and the clone was removed. This is not complete Linux epoll
conformance.
