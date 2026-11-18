# ARM64 `clock_gettime` (#113)

The existing main dispatch mapped supported Linux clock IDs to Darwin clocks
and wrote the resulting 16-byte timespec directly through a guest pointer.
This branch stages the output in host memory and uses the checked guest
copyout path. Unsupported IDs still return `EINVAL`; NULL and unmapped output
addresses now return `EFAULT` without direct memory access. Host query errors
remain propagated.

The disposable ARM64 probe checks realtime, monotonic, boottime, process CPU,
and thread CPU clocks; nanosecond range; a 16-byte output boundary; invalid
clock ID; and invalid output addresses. It does not assert that Darwin's
underlying clock epochs, suspend behavior, or precision exactly match Linux.
The existing clock ID mapping is unchanged.
An isolated private Arch clone reached a fresh login prompt with D-Bus broker
and launcher settled. Its source copy warned about protected `/run` entries;
this remains a boot smoke test, not proof of complete rootfs fidelity.

```sh
./build.sh cli
python3 tests/run-clock-gettime.py --sysroot /path/to/arch-rootfs
```
