# sched_rr_get_interval (arm64 syscall 127)

The old implementation wrote a fixed 1 ms timespec through an unchecked guest
pointer. Shimmy reports `SCHED_OTHER` for guest threads but does not model a
Linux fair runqueue or per-task slice, so that answer was not meaningful.

This bounded implementation validates the guest task ID and timespec address,
then returns `EOPNOTSUPP` for existing tasks without writing an interval.
Negative IDs return `EINVAL`, unknown IDs return `ESRCH`, and null or unmapped
output addresses return `EFAULT`. Linux derives the interval from scheduler
state, and it can be zero on an otherwise idle fair runqueue; host timing is
not a substitute for this guest state.

Build with `./build.sh cli` and run
`python3 tests/run-sched-rr-get-interval.py` for the disposable ARM64 guest
contract test.
