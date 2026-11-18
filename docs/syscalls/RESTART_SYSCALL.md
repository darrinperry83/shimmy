# ARM64 restart_syscall (#128)

Linux invokes the current thread's `restart_block.fn`. With no pending restart block, that function is `do_no_restart_syscall`, which returns `EINTR` ([Linux signal source](https://github.com/torvalds/linux/blob/master/kernel/signal.c)).

Shimmy currently has no guest restart-block registry. Its interrupted `nanosleep` returns host `EINTR`, and `clock_nanosleep` copies remaining time for relative sleeps and also returns `EINTR`. Guest signal frames are not constructed (`rt_sigreturn` is unreachable). A stand-alone call to #128 therefore has no saved syscall, deadline, clock, or remaining time to resume. Returning `EINTR` is the bounded, safe behavior; treating #128 as success or replaying the last syscall would be false.

Full support needs a per-guest-thread restart block, populated only for restartable operations, with clock-specific remaining/deadline state and signal-delivery integration. It must clear after use, thread exit, or a non-restartable interruption. Until that mechanism exists, #128 remains a documented no-block subset. `python3 tests/run-restart-syscall.py` verifies the direct ARM64 guest call returns `EINTR`.
