# `setitimer` (native ARM64 syscall 103)

Shimmy's prior handler dereferenced both guest pointers directly. It also
passed a null new-value pointer to Darwin; Linux's
[`setitimer` implementation](https://github.com/torvalds/linux/blob/master/kernel/time/itimer.c)
instead treats null as a zero timer that disarms the selected timer.

The bounded fix copies the Linux input structure into host memory, validates
both timevals before any state change, and passes the converted value to the
host. It converts the old timer into a host-side Linux structure and uses a
checked guest copyout. As on Linux, a bad old-value pointer can report
`EFAULT` after the timer has changed. The null new-value case now passes a
zero timer to the host. The three existing timer classes remain backed by
Darwin's `setitimer`; no new signal, fork, or scheduling behavior is claimed.

`python3 tests/run-setitimer.py` compiles a freestanding ARM64 guest. It
checks a long virtual timer's arm/disarm and old value, bad input timevals,
invalid guest addresses, and invalid selectors. `./build.sh cli` and the
guest probe passed in the isolated worktree. A private Arch boot using the
current `task-3` fixture reached a fresh login prompt with the D-Bus broker
and launcher settled. Its scoped guest and exact clone were cleaned afterward.
