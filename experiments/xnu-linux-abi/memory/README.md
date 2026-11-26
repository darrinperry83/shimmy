# Linux memory calls backed by XNU

This is the first memory gate for [XNU serving Arch's Linux ABI](../LINUX-PERSONALITY.md).
Linux `mmap` (222), `mprotect` (226) and `munmap` (215) enter the kernel Linux
personality and call XNU's native operations on the process's task map.
XNU owns the actual mappings, protection checks, faults and eventual cleanup.
The original Linux instructions are unchanged.

Initial mapping scope: `MAP_PRIVATE | MAP_ANONYMOUS`, non-fixed placement,
offset zero, ordinary data protection flags. Other mapping kinds return Linux
`EOPNOTSUPP` explicitly. This does not establish file mappings, shared/fixed
mapping semantics, heap growth, COW, executable/JIT mappings or a full shell.
The current ELF task map uses 16 KiB pages; finer-grained Linux mappings remain
an explicit compatibility issue. The fixture uses 16 KiB-aligned subranges and
also passes on the real Linux reference's page size.

The test inherits the existing shim's memory contracts and fault-testing approach
from `src/gmem.c` / `src/test_gmem.c`. Its ELF uses kernel copyin/copyout to verify
real VM permissions: read-only pages reject copyout, inaccessible/unmapped pages
reject copyin, contents survive a NONE→RW protection transition, and partial
unmap leaves the other part accessible. It checks zero fill, holes, repeated
unmap, alignment and zero-length errors. A native parent verifies the transcript,
exit status and its own process identity.

```sh
python3 experiments/xnu-linux-abi/memory/verify.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-memory
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-memory
python3 experiments/xnu-linux-abi/stage_vm_variant.py memory
```

Build/link/stage do not install or boot. The candidate is separate from both
the working PTY collection and the hosted Linux-core collection. The staged VM
job verifies its kernel UUID, runs P0/static ELF/original Arch loader/PTY controls,
then the memory fixture, then renders the same test through the Apple display
output host with WindowServer absent. Runtime results belong in `evidence.json`.

Runtime result: **passed** on kernel UUID `B1187A89-F3A7-3D26-B33A-EA0BDD63D10A`.
The Linux ELF digest matches the real-Linux reference. P0, static ELF, original
Arch loader, native PTY and WindowServer-absence controls pass in the same boot.
The memory result was visually verified on the Apple display output path. See
[evidence.json](evidence.json). The physical Mac's boot configuration is unchanged.
