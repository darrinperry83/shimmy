# ARM64 #79 `newfstatat`: checked guest output

The published handler wrote a 128-byte Linux `stat` result directly through
the guest's non-null output pointer, including synthetic `/proc`, `/dev`,
`/sys` and cgroup branches. A mapped but read-only or invalid guest address
could fault the shim instead of returning `EFAULT`.

This correction stages every successful result in host memory and performs
one checked guest copyout. `AT_EMPTY_PATH` uses synthetic descriptor ownership
for device overlays rather than reading `gabs`, which is not initialized when
no path is resolved. Virtual tty identity is kept consistent between pathname
and fd forms.

`python3 tests/run-newfstatat-checked.py` compiles a disposable AArch64 guest
probe covering an ordinary file, `AT_EMPTY_PATH` with empty and null paths,
null and non-null invalid output addresses, synthetic `/proc/self/mounts`,
and a virtual tty by both pathname and descriptor. It checks mode, size,
identity and a canary beyond the Linux stat struct. The CLI build passes.

This is an output-memory and synthetic-identity correction. It does not
establish complete pathname input validation, all flag combinations, or
Linux permission and mount-namespace parity.
