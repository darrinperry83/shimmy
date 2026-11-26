# M1: a static Linux ELF on XNU

The minimal runtime checkpoint passes in the disposable VM. This is a static
AArch64 ET_EXEC with no libc, interpreter, relocations or TLS. It uses ordinary
Linux SVC #0 with numbers in x8, without the P0 x16 marker. No executable bytes
are rewritten. The same ELF runs on real AArch64 Linux as a reference.

## Reproduce build and local checks

From the repository root:

```sh
python3 experiments/xnu-linux-abi/elf/verify.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-elf
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-elf
python3 experiments/xnu-linux-abi/stage_vm_elf.py
```

The reference command requires the already-installed Docker image
menci/archlinuxarm:base-devel and records its content identity. It runs without
network access and mounts only this experiment read-only. Omit the reference
option to perform local fixture and sanitizer tests only. The build requires
the previously prepared pinned XNU/KDK dependencies documented in VM-TESTING.md.

## Implementation

elf_plan.h reuses the ELF64 format definitions and 16KB mapping geometry from
src/elfload.c, with a separate strict validator suitable for kernel input. It
checks bounded headers/segments, permissions, alignment, overlap, file and
address bounds, entry point, and mapped program headers. Unsupported dynamic,
TLS, interpreter and executable-stack images fail. Host tests use the actual
planner, malformed inputs and 20,000 deterministic mutations under ASan/UBSan.

elf_prepare.py generates exact-source edits under the existing build lock.
They attach the ELF loader to XNU's existing exec transaction and preserve its
credential/descriptor/map lifecycle. A local Mach header descriptor selects
shared architecture housekeeping; it does not replace or rewrite the ELF file.
The builder restores all edited source files after building.

linux_exec.h maps zeroed anonymous segments, copies their original bytes,
applies final and maximum permissions, and constructs argc/argv/envp plus a
minimal Linux auxv. Process personality uses a reserved bit in the pinned
p_ladvflag without changing proc layout. Fork copies that bit, successful
Darwin exec clears it, and Linux SVC dispatch uses it. Only getpid, write and
exit/exit_group are implemented; other Linux syscall numbers return -ENOSYS.

The experiment is built only in the isolated DEVELOPMENT VMAPPLE variant and
requires rdxnu_linux_elf=1. It allows unsigned ELF execution in that experiment's
maps rather than applying Darwin Mach-O signature validation to them. Existing
MAC errors are rejected. This is an experimental execution policy, not the
final security design. The VM still needs the diagnostic BTI workaround.

## Guest execution and evidence

The staging command writes local results files only. Guest installation remains
a separate guarded Recovery operation against rdxnu-25f80, with the guest Data
volume unlocked. Install elf-probe, elf-run-probe, elf-boot.sh under
/Library/RDXNU-P0, and elf-job.plist as the root-owned mode-0644
/Library/LaunchDaemons/org.rdxnu.m1.plist. Preserve the old P0 job in its data
folder rather than running it with the old expected UUID. The runner also
requires the existing /Library/RDXNU-P0/probe for its Darwin/P0 control.

Configure the matching collection only inside the disposable guest Recovery.
Add rdxnu_linux_elf=1 alongside rdxnu_linux_probe=1; do not alter the host boot
policy. The existing experimental WindowServer/loginwindow disable overrides
remain in force for this test. They are not the final service replacement.

The runner records kern.uuid and kern.kernelcacheuuid, runs the Darwin carrier
and native ELF harness, and exports /var/tmp/rdxnu-elf.log to results/elf-boot.log.
The ELF reports its own PID and a magic value through a pipe; the native parent
checks both against the actual child and requires exit status 37. The ELF checks
initial stack/auxv, BSS, getpid, -ENOSYS, -EFAULT, -EBADF and write. The parent
checks that its own native PID remains intact. The runner records process state
and powerd after startup. A saved preboot image on the VM display is not proof
that WindowServer is executing.

Machine-readable runtime proof: out/vmapple/elf-runtime-result.json. General
Linux semantics, dynamic glibc/Arch applications, Linux fork/exec ABI, signals,
physical hardware and full WindowServer service replacement remain unproven.
