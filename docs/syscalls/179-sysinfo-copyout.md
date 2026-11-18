# ARM64 #179 `sysinfo`: checked guest output

The published handler fills a synthetic Linux `sysinfo` structure directly
through the guest pointer. An invalid non-null pointer could fault the host
shim. This isolated change builds the same structure locally and copies it to
admitted guest memory with `gmem_guest_copyout_checked`. A null or unmapped
pointer returns `EFAULT` without a host write. All existing field formulas
and values are preserved; this is an output-boundary correction only.

`./build.sh cli` and `python3 tests/run-sysinfo-copyout.py` pass. The raw
AArch64 probe checks the existing field relationships, unchanged adjacent
sentinel, NULL, unmapped pointer, and repeat normal output. A private Arch boot reached a fresh LOGIN prompt with settled D-Bus; the
exact guest and disposable rootfs clone were cleaned. This is a startup
smoke gate, not resource-accounting validation.

This does **not** make the synthetic resource values accurate. The existing
`freeram = totalram / 4`, `procs = 1`, zero swap, and eight-GiB fallback
remain. A separate refusal candidate returned `EOPNOTSUPP` but caused Arch
init to abort before login; see `179-sysinfo-blocker.md`. Guest resource
accounting and a boot-compatible policy remain safeguarded work.
