# Direct Arch ELF execution and native root scope

The opt-in `--with-linux-interp` profile extends the native process checkpoint.
It implements ELF `PT_INTERP` so a caller can execute `/usr/bin/printf` or
`/usr/lib/systemd/systemd` directly. The interpreter path is resolved by XNU's
vnode lookup in the calling process's real root and credentials. There is no
userspace loader wrapper, syscall interception, path prefix rewrite or PID
substitution. Original Arch program, loader and library bytes stay unchanged.

Live kernel `10A390BE-A623-3DF9-8337-20BD4F3C71DD` passes this gate and
all preceding regressions on the same boot. Direct-exec stdout and stderr
match the pinned Linux reference exactly. The proof is visible through the
Apple display driver without WindowServer. See [evidence.json](evidence.json).

Before replacing the task, the activator looks up and holds the interpreter
vnode, reuses native exec/MAC authorization, rejects non-executable/noexec and
set-id interpreters, and validates its bounded ELF plan. Every activator exit
releases the opened vnode. Missing, denied and short interpreter failures return
without replacing the caller. Segment allocation, protection and page faults
belong to the XNU map. The interpreter enters first; AT_BASE identifies its
independent load bias, while AT_PHDR/PHNUM/ENTRY identify the main executable.
AT_EXECFN contains the actual exec path, independently of argv[0]. The unchanged
Arch loader performs relocations, TLS initialization and library loading.

The native test supervisor stays outside the execution root. Each test child
uses native chroot/chdir and drops supplementary groups and uid/gid to 65534
before exec. The minimal root contains the already pinned Arch dependency
closure, original printf, the probe and deliberate interpreter error fixtures.
The root's `/lib` symlink follows Arch layout. Its lack of `/proc` causes a
systemd version warning on Linux; stderr is compared separately and exactly.
The existing outside-root version regression must still have empty stderr.
This tests path scope, not a complete security sandbox or a Linux namespace.

```sh
python3 experiments/xnu-linux-abi/interp/verify.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-interp
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-interp
python3 experiments/xnu-linux-abi/stage_vm_variant.py interp
python3 experiments/xnu-linux-abi/startup/verify_runtime.py <unique-staged-name>
```

Host planner controls use ASan/UBSan and check malformed, duplicate and nested
interpreter metadata. The same dynamic Linux probe on both kernels verifies
auxv, custom argv[0], environment, initialized TLS, inaccessible outside-root
paths, exec error preservation of TLS, fork, direct original Arch child exec
and exact reap. Native parent controls and all earlier runtime gates remain
required. Runtime acceptance is recorded separately from build/reference tests.

This is a DEVELOPMENT/VMAPPLE diagnostic with fixed, bounded ELF load addresses
and copied segments. It does not establish ASLR, executable-file mutation/text
locking, every ELF/TLS format, Linux PID/init/mount namespaces, a complete procfs,
a running systemd manager, an interactive shell or physical hardware support.
Interpreter paths must be absolute, NUL terminated, at most 255 bytes, and in
the captured header page; nested interpreters and static TLS are rejected.
Systemd's existing unbooted-user-manager diagnostic remains a separate negative
control. This profile establishes a normal executable-path prerequisite for
services; it does not bypass systemd's boot checks.
