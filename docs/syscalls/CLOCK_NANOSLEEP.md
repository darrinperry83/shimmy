# ARM64 `clock_nanosleep` (#115)

The existing handler read the guest request and wrote the relative remainder
through raw pointers, accepted unknown flag bits, and ignored a host clock
query failure for absolute sleeps. This branch stages the 16-byte request via
checked guest copyin, validates `tv_sec` and `tv_nsec`, accepts only `0` or
`TIMER_ABSTIME`, and copies a relative `EINTR` remainder through checked guest
memory. It checks the host clock query before subtracting an absolute
deadline and returns immediately for a past or equal deadline.

Only the existing realtime, monotonic and boottime clock mappings remain
eligible. Thread CPU time is invalid for Linux `clock_nanosleep`. Process CPU
time and coarse/raw clocks return `EOPNOTSUPP`: a Darwin wall-time sleep would
falsely claim to have waited for their requested clock. The absolute realtime
path still converts a deadline to a single relative host sleep, so a host
wall-clock adjustment during that sleep is not fully modeled. Guest signal
delivery, `EINTR` remainder behavior, and restart blocks are not fully
verified; the call must not be described as complete Linux compatibility.

The disposable ARM64 probe covers relative sleep, absolute future/past
realtime and monotonic deadlines, valid zero-duration calls, invalid flags,
invalid or unmapped request pointers, invalid timespecs, refusal of unsupported
CPU clocks, and unused remainder pointers on successful/absolute calls.
An isolated private Arch clone reached a fresh login prompt with the D-Bus
broker and launcher settled. The fixture copy warned about protected `/run`
entries, so this is a boot smoke test rather than complete filesystem proof.

```sh
./build.sh cli
python3 tests/run-clock-nanosleep.py --sysroot /path/to/arch-rootfs
```
