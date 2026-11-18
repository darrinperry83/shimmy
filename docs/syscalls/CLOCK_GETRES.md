# ARM64 `clock_getres` (#114)

The existing main dispatch recognized several Linux clock IDs but wrote the
result directly to a guest pointer. It also replaced any host query failure
with a fabricated one-nanosecond resolution. This branch retains the existing
clock mapping, checks the output memory, and reports a host query failure
instead of inventing a resolution. A null output pointer is allowed after
clock validation, as on Linux.

The ARM64 probe checks realtime, monotonic, process CPU, and thread CPU clocks;
valid null output; an invalid clock ID; an invalid output address; and that a
write does not overrun the 16-byte Linux timespec. It does not claim that
Darwin's clock resolution equals Linux hardware clock precision. A private
Arch clone reached a fresh login prompt with the D-Bus broker and launcher
settled; its copy reported permission errors for protected source entries,
so this is a boot smoke test rather than proof of full rootfs fidelity. The existing
coarse and boottime clock mappings remain approximations and were not changed.

```sh
./build.sh cli
python3 tests/run-clock-getres.py --sysroot /path/to/arch-rootfs
```
