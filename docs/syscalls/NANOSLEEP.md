# ARM64 `nanosleep` (#101)

The old extended handler passed guest request and remainder pointers directly
to Darwin `nanosleep`. This branch stages the 16-byte ARM64 timespec through
checked guest memory, validates nonnegative seconds and nanoseconds below one
billion, and copies the remaining interval back only on `EINTR`. A successful
sleep leaves the remainder buffer unchanged. Host sleep errors are translated
to Linux errno without a direct guest pointer write.

The disposable ARM64 guest probe checks a short elapsed sleep, unchanged
remainder on success, NULL and invalid request pointers, invalid timespec
values, and an invalid remainder pointer on an uninterrupted zero-duration
sleep. Interruption and guest signal delivery are not proven by this test;
the existing signal virtualization and restart-block limitations remain.
An isolated private Arch clone reached a fresh login prompt with D-Bus broker
and launcher settled. The source copy warned about protected `/run` entries;
the boot result is a smoke test, not full rootfs fidelity proof.

```sh
./build.sh cli
python3 tests/run-nanosleep.py --sysroot /path/to/arch-rootfs
```
