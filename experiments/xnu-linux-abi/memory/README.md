# Linux memory calls backed by XNU

This is the first memory gate for [XNU serving Arch's Linux ABI](../LINUX-PERSONALITY.md).
Linux `mmap` (222), `mprotect` (226) and `munmap` (215) enter the kernel Linux
personality and call XNU's native operations on the process's task map.
XNU owns the actual mappings, protection checks, faults and eventual cleanup.
The original Linux instructions are unchanged.

The initial checkpoint below covered `MAP_PRIVATE | MAP_ANONYMOUS`, non-fixed
placement, offset zero and ordinary data protections. The filemap extension
adds private file mappings and fixed replacement, described below. Shared maps,
NOREPLACE and unverified flags return Linux `EOPNOTSUPP` explicitly. Heap growth,
fork COW, JIT mappings and a full shell remain unproven.
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

## Private file mappings and fixed replacement

`--with-linux-filemap` builds a separate `-memory-filemap` kernel candidate,
preserving the original memory checkpoint. Linux `MAP_PRIVATE` file mappings
use native XNU descriptor, vnode, pager and copy-on-write behavior. Linux's
`MAP_FIXED` and `MAP_ANONYMOUS` values are explicitly converted to Darwin flags;
XNU owns the replacement and backing map. File offsets and fixed addresses must
align to the task's 16 KiB page size. Shared mappings, NOREPLACE and other flags
are still rejected.

`file_probe.S` maps an inherited regular-file descriptor, checks its first and
last data bytes, writes privately and verifies the original file bytes through
`read`. It maps the second file page RX and executes an unchanged AArch64
`mov w0,#37; ret` sequence. It then replaces part of an anonymous reservation
with a read-only file mapping, checks protection and its untouched neighbor,
replaces the file mapping with zero-filled anonymous memory at the same address,
and tests invalid offsets, bad descriptors and directory mappings. The native
parent compares the entire file against its initial contents after child exit.
Descriptors are inherited from the test parent; this does not prove Linux
`openat`, full ELF dynamic linking, fork COW or loader startup.

The live test exposed a native semantic difference: XNU rejects directory
mapping with `EINVAL`, while Linux uses `ENODEV`. The overlay changes that error
inside XNU's existing vnode-validation path only for the Linux personality.
It keeps the existing descriptor reference and cleanup, avoiding a separate
preflight lookup that could race descriptor reuse. The native test parent
also checks that Darwin's directory-mapping error remains `EINVAL`.

```sh
python3 experiments/xnu-linux-abi/memory/verify_filemap.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-filemap
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-filemap
python3 experiments/xnu-linux-abi/stage_vm_variant.py filemap
```

The staged guest job runs all earlier controls and the original anonymous-memory
fixture before the filemap fixture. Live results are recorded separately in
`filemap-evidence.json`; a successful build/reference alone is not a live pass.

Runtime result: **passed** on kernel UUID `238B1961-DF68-30ED-9368-DD2E7B7C35D3`.
The unchanged ELF passes on both real Linux and the live XNU candidate, with all
earlier controls passing in the same boot. The test result is also visible on
the Apple display output path. See [filemap-evidence.json](filemap-evidence.json).
