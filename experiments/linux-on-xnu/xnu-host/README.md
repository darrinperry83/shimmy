# Freestanding Linux core in XNU: diagnostic backend

This is the next experiment after [the native library smoke](../core-smoke/README.md).
It links reused Linux code into an isolated DEVELOPMENT VMAPPLE XNU kernel.
The VM is a disposable test machine, not the proposed final Linux runtime.
The final design still has no guest Linux VM.

`build_core.py` compiles the pinned Darwin/LKL filesystem profile (714 C units)
for ARM64e using Xcode's kernel ABI. It excludes the POSIX host objects and
libSystem, disables implicit FP/SIMD and outline atomics, and prefixes Linux
symbols to prevent collisions with XNU. The source pin is
`c4d0895ead9badb52f7bbffe3d529c61c8fc9485` (Linux 6.12.0).
The optional Linux `counted_by` annotation is disabled for this older source
with Apple's partially supporting compiler; object layouts are unchanged.

`rdxnu_linux_host.c` supplies XNU allocation, semaphore, recursive mutex, thread,
TLS, timer and context callbacks. The architecture-specific jump helper keeps
the saved stack frame live while its callback runs. No Linux device is attached.
The test calls the core directly using its pinned internal syscall API with
kernel-owned buffers. It does **not** redirect an ELF process's SVCs into it.

Build locally:

```sh
python3 experiments/linux-on-xnu/xnu-host/build_core.py
python3 -m unittest discover -s experiments/linux-on-xnu/xnu-host -p 'test_*.py' -v
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-core
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-core
python3 experiments/linux-on-xnu/xnu-host/stage_vm.py
```

The existing prepared XNU/KDK and pinned Darwin/LKL source checkout are required.
The builder temporarily stages the backend and restores all XNU source changes
under the shared build lock. `stage_vm.py` prepares both the candidate and a
separate known-working console rollback in the VM results share. It does not
install either. Recovery installers verify VMAPPLE, virtualization, root, the
expected recovery system metadata and the exact collection digest before use.
Credentials are supplied interactively/in the one-time invocation, never saved.

Runtime gate: the candidate needs `rdxnu_linux_core=1`, followed by a root write
of `1` to `kern.rdxnu_linux_core`. Reads return 0 idle, 1 running, 2 passed or a
negative failure. It cannot be restarted in the same boot. The staged root job
checks the kernel UUID and runs the existing P0, ELF, Arch loader and PTY controls
before triggering the core test. The test checks Linux pipe/EAGAIN/EOF/EBADF,
epoll readiness and draining, RAMFS create/write/seek/read/remove, then orderly
Linux shutdown. No existing XNU fd is passed to Linux.

A built/linked collection alone is not a runtime pass. Consult `evidence.json`
for the observed result. Full ELF process execution through this core, user/kernel
memory isolation, fork/exec, signals, SMP, an Arch shell and systemd remain work.
The source profile's `/init` startup message is not Arch's init/systemd.
