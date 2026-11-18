# `munlockall` (ARM64 syscall 231): guest-ledger unlock subset

`munlockall` previously returned `ENOSYS` because walking only the guest range
ledger was judged insufficient: `mlock` (#228) could lock a guest-visible mapping
the ledger did not include, and unconditionally unlocking the whole host process
would release Shimmy-private mappings.

With the guest-ledger enumeration added for `mlockall`, `munlockall` now:

- clears the `MCL_FUTURE` policy,
- snapshots the guest range ledger, and
- `munlock`s every current guest span.

`munlock` of a never-locked page is a no-op on both Linux and macOS, so walking
the whole ledger releases exactly what `mlock`/`mlock2`/`mlockall` pinned without
touching non-ledger (Shimmy-private, DRM, or ALSA) memory. DRM/ALSA mappings are
outside the ledger and were never lockable through #228, so there is nothing of
theirs to release.

## Known deviations

- A span unmapped between snapshot and unlock is skipped (Darwin `munlock`
  returns `ENOMEM` there and the kernel already released the lock).
- The ledger is the same bounded view as `mlockall`; it does not include SysV
  SHM (which is refused) or device-backed mappings.

## Validation

`./build.sh cli` and `python3 tests/run-munlockall.py` pass. The probe locks a
guest mapping, verifies `munlockall` now succeeds (and is idempotent), then
unmaps. A live systemd/greeter boot gate is not run here.

Primary source: [Linux `mm/mlock.c`, `SYSCALL_DEFINE0(munlockall)`](https://github.com/torvalds/linux/blob/master/mm/mlock.c).
