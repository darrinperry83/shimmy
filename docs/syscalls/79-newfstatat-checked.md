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

The later input correction snapshots a non-null guest pathname through
checked memory before testing for an empty path or calling the resolver.
NULL remains valid with `AT_EMPTY_PATH`; without that flag, NULL returns
`EFAULT`. An unmapped non-null path returns `EFAULT` in either form.

`python3 tests/run-newfstatat-checked.py` compiles a disposable AArch64 guest
probe covering an ordinary file, `AT_EMPTY_PATH` with empty and null paths,
null and unmapped invalid input paths, null and non-null invalid output
addresses, synthetic `/proc/self/mounts`,
and a virtual tty by both pathname and descriptor. It checks mode, size,
identity and a canary beyond the Linux stat struct. The CLI build passes.
A private Arch clone reached a fresh login prompt with D-Bus broker and
launcher settled both for the output correction and for the later input
correction; the exact test guests and disposable clones were cleaned.

This is a bounded input/output-memory and synthetic-identity correction. It
does not establish all flag combinations, concurrent path replacement, or
Linux permission and mount-namespace parity.
