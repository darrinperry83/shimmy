# `getitimer` (native ARM64 syscall 102)

The existing handler used the host's `getitimer` result, but wrote through the
guest pointer directly and reported success for a null destination. Linux's
[`getitimer` implementation](https://github.com/torvalds/linux/blob/master/kernel/time/itimer.c)
checks the timer selector and then copies the result to the user pointer,
returning `EFAULT` when that copy fails.

The bounded fix keeps the existing host timer query and Linux structure
conversion, stages the result in host memory, and uses Shimmy's checked guest
copyout. A null or invalid guest destination now returns `EFAULT`; an invalid
timer selector still returns `EINVAL` first. It does not change `setitimer`
(103), timer scheduling, signal delivery, or process inheritance.

`python3 tests/run-getitimer.py` compiles a freestanding ARM64 guest. It checks
the three valid selectors, converted time fields, null and invalid pointers,
and invalid selectors. `./build.sh cli` and the guest probe passed in the
isolated worktree. A private Arch boot using the current `task-3` fixture also
reached a fresh login prompt with the D-Bus broker and launcher settled. Its
scoped guest process and cloned rootfs were cleaned afterward.
