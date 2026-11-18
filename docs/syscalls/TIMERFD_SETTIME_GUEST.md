# `timerfd_settime` (native ARM64 syscall 86): guest memory boundary

The existing syscall wrapper passed guest `new_value` and `old_value`
pointers directly to the timerfd backend. The backend dereferenced them as
host pointers. Linux's [implementation](https://github.com/torvalds/linux/blob/master/fs/timerfd.c)
first copies the new value from userspace, changes the timer, then copies an
optional old value back. A failed old-value copy can therefore return
`EFAULT` after the timer changed.

This change stages the Linux itimerspec in host memory with checked guest
copyin, passes only host storage to the existing backend, and uses checked
guest copyout for an optional old value. It retains the current timerfd
backend's clocks, flags and scheduling behavior; it does not claim broader
Linux timerfd or signal parity. `timerfd_gettime` (87) is unchanged.

`python3 tests/run-timerfd-settime-guest.py` compiles a freestanding ARM64
guest. It arms and disarms a monotonic timer, checks old-value readback,
invalid new and old guest addresses, invalid flags and bad fd behavior. The
CLI build and guest probe passed. A private Arch boot using the current
`task-3` fixture reached a fresh login prompt with D-Bus broker and launcher
settled. Its scoped guest and exact clone were cleaned afterward.
