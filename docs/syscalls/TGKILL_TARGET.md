# `tgkill` (native ARM64 syscall 131): group and thread identity

Linux's [signal implementation](https://github.com/torvalds/linux/blob/master/kernel/signal.c)
checks that `tgid` and `tid` are positive and that the target thread belongs
to the named thread group. Signal zero still performs that existence check.
Shimmy's prior shared path used the group ID as the target and returned
success for signal zero without checking the thread ID.

The bounded correction reads the target from the second argument, requires
the current guest process group and a known guest thread, then uses the
existing signal path. Unknown groups or threads return `ESRCH`; nonpositive
IDs return `EINVAL`. It does not claim Linux thread-directed delivery for
positive signals: the existing host path still directs those to Shimmy's
process. It leaves `tkill` (130) unchanged.

`python3 tests/run-tgkill-target.py` builds a freestanding ARM64 guest and
checks its own group/thread IDs, nonpositive IDs, mismatched group, unknown
thread, and invalid signal. `./build.sh cli` and this probe pass in the
isolated worktree. A private Arch boot using the current `task-3` fixture
reached a fresh login prompt with the D-Bus broker and launcher settled. Its
scoped guest and exact clone were cleaned afterward.
