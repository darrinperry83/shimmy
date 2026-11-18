# ARM64 #139 `rt_sigreturn`: signal-frame blocker

`rt_sigreturn` restores a signal handler's saved register state and mask from
the guest signal frame. Shimmy does not yet construct guest handler frames or
retain that saved state. The previous core dispatcher returned success for a
raw guest call even though it had restored nothing. It now returns `ENOSYS`.

The disposable ARM64 probe `tests/guest_rt_sigreturn_unavailable.c` checks
this refusal through the actual guest syscall path. This is not a working
`rt_sigreturn` implementation; it makes the existing limitation explicit.
