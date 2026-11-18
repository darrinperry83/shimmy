# ARM64 #192 `semtimedop`: host semaphore ID and timeout blocker

Darwin SysV semaphore IDs are host-global. Shimmy's #190 `semget` already
returns `ENOSYS` because it cannot mint instance-scoped guest semaphore IDs.
The published #192 handler nevertheless passed any guest-supplied ID and raw
guest operation pointer to host `semop`. It also ignored the guest timeout,
which could turn a finite wait into an indefinite host wait.

This safeguard returns `ENOSYS` before reading guest memory or calling host
IPC. It is a refusal, not a working semaphore operation. The disposable
ARM64 probe `python3 tests/run-semtimedop-refused.py` exercises finite and null
timeouts, arbitrary IDs, and invalid pointers without host semaphore calls.

Real support needs a private semaphore ID registry, checked operation arrays,
atomic operation semantics, timeout behavior and lifecycle across #190–193.
At this branch base, #191 and #193 still accept raw host IDs.
