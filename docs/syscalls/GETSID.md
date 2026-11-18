# `getsid` (#156) bounded guest PID query

Linux resolves `getsid(pid)` in its process namespace and returns the session
leader's PID. Shimmy previously passed the guest's number directly to Darwin
`getsid`, which could inspect an unrelated Mac process with the same PID.

This implementation accepts `pid=0` and the current guest PID only. It asks
Darwin for the current shim process's session, then returns the virtual guest
PID only when the leader is this process, or the known immediate guest parent
when that parent is the host session leader. An unrelated or deeper ancestor
session returns `EOPNOTSUPP` rather than a host PID. A known guest child query
also returns `EOPNOTSUPP`; an unknown PID returns `ESRCH`. It does not model
cross-process session lookup or alter `setpgid`/`setsid`.

The disposable ARM64 test checks an unrelated **live host PID** cannot be
queried, `setsid`/self readback, child inheritance, and known-child refusal.
`./build.sh cli` and `python3 tests/run-getsid.py` passed locally. A separate
Arch boot gate is still needed because an initial inherited host session now
returns `EOPNOTSUPP` instead of leaking its host leader PID.

Reference: [Linux `getsid` implementation](https://linux.googlesource.com/linux/kernel/git/torvalds/linux/+/1d227fcc72223cbdd34d0ce13541cbaab5e0d72f/kernel/sys.c).
