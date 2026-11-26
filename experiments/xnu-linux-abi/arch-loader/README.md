# First unmodified Arch component: standalone dynamic loader

This isolated follow-on to M1 adds ET_DYN load bias, preserves dynamic metadata
for the loader's self-relocation, and supplies UID/GID, random, executable name
and conservative FP/ASIMD auxiliary-vector entries. It adds Linux writev and
identity syscalls needed by the test. The M1 source and successful collection
remain separate.

The first target is the unchanged Arch Linux ARM glibc package's
ld-linux-aarch64.so.1 --help. That is a useful intermediate checkpoint, not
proof that an application with PT_INTERP or libc dependencies can run.
PT_INTERP and TLS segments are still rejected. The kernel does not relocate the
loader; the original loader code handles its own dynamic relocation.

Build the separate candidate with:

```sh
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-arch-loader
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-arch-loader
```

Output: out/vmapple/baseline-no-lto-no-bti-svc-probe-elf-arch-loader.
The same DEVELOPMENT/VM-only and unsigned-ELF diagnostic limitations as M1
apply. This is not installed by the build/link commands.

The reference loader and true binary were copied without modification from the
existing menci/archlinuxarm:base-devel image. Package versions and SHA-256 digests
are recorded in out/arch-loader/package-manifest.json. The loader --help output
on real Linux is recorded in out/arch-loader/linux-reference.log. The planner
accepts that loader and rejects the true binary's unsupported PT_INTERP path;
malformed input mutations are exercised under ASan/UBSan.

The native run_arch.c parent executes a command, enforces a timeout and checks
its own process identity and child exit status. guest_boot.sh runs the earlier
P0/M1 regressions before the unmodified Arch loader and records its file hash.
Runtime success requires a matching live kernel UUID, package hash, actual help
output and zero child status in results/loader-boot.log; a build is insufficient.

No new display backend is enabled here. The VM can retain its saved preboot
image while these headless service tests run. Output in a log is not guest
screen rendering, a Linux console or an Arch desktop.


The standalone --help command now passes on XNU. The guest file hash matches
the package and its help output matches reference Linux after argv[0]
normalization. P0/M1 controls pass on the same boot. Runtime proof is
out/vmapple/arch-loader-runtime-result.json. This does not establish brk,
shared libraries or a dynamically linked application: brk still returns
-ENOSYS, and the successful help path does not need a working heap.
