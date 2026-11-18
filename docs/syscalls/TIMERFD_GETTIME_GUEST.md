# `timerfd_gettime` (native ARM64 syscall 87): guest output

The prior syscall wrapper passed the guest output pointer directly to the
timerfd backend. The backend then wrote through it as a host pointer. Linux's
[`timerfd_gettime`](https://github.com/torvalds/linux/blob/master/fs/timerfd.c)
queries the timer and copies the result to userspace afterward.

The bounded correction stages the Linux itimerspec in host memory and uses
checked guest copyout only after a successful backend query. Thus an invalid
fd still returns `EBADF` before an invalid output pointer is considered;
a valid fd with a null or unmapped output returns `EFAULT`. Existing timerfd
clocks and scheduling remain unchanged. `timerfd_settime` (86) is separate.

`python3 tests/run-timerfd-gettime-guest.py` compiles a freestanding ARM64
guest and checks an armed monotonic timer, null/unmapped output, invalid fd
ordering, and disarmed readback. The CLI build and guest probe passed. A
private Arch boot using the current `task-3` fixture reached a fresh login
prompt with the D-Bus broker and launcher settled. Its scoped guest and
exact clone were cleaned afterward.
