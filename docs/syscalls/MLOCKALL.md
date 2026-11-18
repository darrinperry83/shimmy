# `mlockall` (ARM64 syscall 230): per-range guest-mapping subset

The previous handler forwarded Linux `MCL_*` flags straight to Darwin
`mlockall()`. On this Mac that host call is `ENOSYS`, which is what made stock
greetd 0.10.3 abort with "unable to lock pages" (see `GREETER_BOOT.md`). Had the
host call ever succeeded it would also have locked Shimmy-private memory outside
the guest, so the passthrough was wrong in both directions.

`mlockall` now locks the guest's own mappings instead of the host process.

## Implemented subset

- Flags are validated: `MCL_CURRENT` (1), `MCL_FUTURE` (2) and `MCL_ONFAULT` (4).
  Unknown bits return `EINVAL`.
- `MCL_ONFAULT` returns `EOPNOTSUPP`: Darwin `mlock` is eager and there is no
  fault-time deferred-lock path to represent it.
- `MCL_CURRENT` snapshots the guest range ledger (`gmem_guest_range_snapshot`),
  re-validates each span as guest-owned and host-mapped, then `mlock`s it.
  Executable spans are skipped (see below). A mid-walk failure rolls back the
  spans already locked before returning the error, so the call is all-or-nothing.
- `MCL_FUTURE` sets a policy flag that `gmem_mmap` consults: every subsequent
  guest mapping is `mlock`'d as it is created (best effort; the mmap itself is
  not failed on a lock error).
- `munlockall` (#231) clears the future policy and unlocks every current guest
  mapping (see `MUNLOCKALL.md`).

## Known deviations from Linux

- **Executable text is skipped.** Apple Silicon serves every executable mapping
  as `MAP_JIT`, and Darwin `mlock()` refuses `MAP_JIT` with `EPERM`, so guest
  text/JIT pages cannot be wired. Data, stack, heap, and read-only mappings are
  locked; text is not. greetd's secret-bearing memory is data/stack, so this does
  not weaken its purpose.
- **Device mappings are not locked.** DRM/ALSA buffers are mapped outside the
  guest ledger and are never lockable through #228/#284 either, so they are
  excluded consistently.
- **Future-lock is inherited across `fork` and not reset on `exec`.** Linux keeps
  `mm->def_flags` across `fork` but clears it on `execve`; the shim does not hook
  the exec path, so a future-lock stays armed after guest exec.
- Host `mlock` resource limits differ from the guest's `RLIMIT_MEMLOCK`/CAP
  accounting, so the exact failure boundary is not Linux-parity.

## Validation

`./build.sh cli` and `python3 tests/run-mlockall.py` pass. The static ARM64 guest
probe covers invalid flags, `MCL_ONFAULT` refusal, zero flags, `MCL_CURRENT`,
`MCL_CURRENT|MCL_FUTURE` (the greetd call), a mapping created under
`MCL_FUTURE`, and `munlockall`. A live greetd/SDDM boot is not run here.

Primary source: [Linux `mm/mlock.c`, `SYSCALL_DEFINE1(mlockall)`](https://github.com/torvalds/linux/blob/master/mm/mlock.c).
