# `setsid` (AArch64 syscall 157)

Shimmy delegates the creation of a session to Darwin `setsid()` **for the
current shim process only**. On success it returns the caller's guest PID,
which is the Linux session ID; this matters for virtual init, whose guest PID
is 1 while its host PID is different. Darwin errors still become Linux errno.

This bounded implementation does not provide a shared guest session registry,
cross-process session queries, or complete Linux job control. It depends on
the scoped `setpgid`, `getpgid`, and `getsid` behavior in the same branch base.

`tests/run-setsid-scope.py` runs a disposable ARM64 guest probe both as an
ordinary guest and as virtual PID 1. Each verifies the return value, self
`getsid`/`getpgid`, and repeat `setsid` failure. It does not exercise the GUI
or another guest process joining the new session.
