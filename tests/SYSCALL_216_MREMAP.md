# ARM64 syscall 216: `mremap`

This batch validates arguments and guest ownership before changing a mapping. A
disposable ARM64 probe covers no-op, shrink, invalid alignment, zero/overflowing
lengths, unsupported flags, unmapped addresses, and mapping preservation after
rejected calls. Run `./build.sh cli` and `python3 tests/run-mremap.py`.

Supported paths remain no-op, shrink, and the existing in-place attempt to add
an anonymous RW extension. The in-place path is retained because Qt QML previously looped
on permanent `ENOMEM` when growth was never attempted.

The current move path is **not** full Linux `mremap`: it allocates anonymous RW
memory, uses a raw host `memcpy`, and does not preserve source backing or
protection. Its behavior on file mappings, PROT_NONE, and concurrent remaps is
not established. `MREMAP_FIXED`, `MREMAP_DONTUNMAP`, and `old_size=0` clone
return `EOPNOTSUPP` rather than silently changing the wrong range. Completing
move semantics needs provenance and protection metadata in the guest mapping
model plus a safe copy transaction. This probe does not validate a systemd or
GUI boot, or attempt to claim all `mremap` variants.
