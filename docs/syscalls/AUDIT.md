# Shimmy Linux ARM64 syscall compatibility audit

Scope: the Shimmy development source and its installed Arch ARM64 syscall header, as inspected on 2026-11-16. This is a **static dispatch inventory**, not a claim that any syscall is behaviorally compatible. Reproduce with `python3 docs/syscalls/inventory.py . rootfs/usr/include/asm/unistd_64.h docs/syscalls/ledger.json`. Passing the installed guest header makes future kernel revisions explicit; the JSON records its SHA-256 and the source commit/dirty state.

## Measured surface

| Measure | Count | Interpretation |
| --- | ---: | --- |
| ARM64 syscall numbers exposed by the installed header | 327 | Header has numbered calls through 471, with reserved gaps. |
| Shimmy dispatch cases in main, extended, or network tiers | 300 | Only means the number reaches a case. It may be a stub, a partial implementation, or a no-op. |
| Header numbers without a dispatch case | 27 | Numbers 444–471, excluding 452 (`fchmodat2`, handled). They return the extended tier's default `-ENOSYS`. |

The complete [machine-readable ledger](ledger.json) assigns each of the 327 calls a mutually exclusive **static triage** state: **209 unknown**, **10 partial or unknown**, **12 known placeholders**, **96 unsupported** (including the 27 without dispatch cases). Every row has dispatch locations, source/version metadata, a priority, and empty test/runtime evidence fields. Every `verified_compatible` field remains false: this generator ran no behavioral conformance tests. These are triage counts, not percentages of Linux compatibility. Several functions contain nested branches or generic synthetic services that a case-label scan cannot certify.

The dispatch route is `src/shim.c:260–289`, then `net_syscall` for network-owned calls or `linux_syscall` and `linux_syscall_ext` for everything else. The trap handler separately implements some ABI-specific behavior, such as `clone` and `execve`, so an extended-tier `-ENOSYS` for `clone` does not mean the guest always receives that result. The generated inventory must be read with that routing in mind.

`tests/test_epoll_pwait2.c` passes unmasked timeout validation, a nanosecond wait, and ready-event transfer on macOS ARM64. It does not verify supplied signal masks or all Linux epoll edge cases. An installed Arch boot also reached login with `systemd-resolved` running after virtual ownership metadata moved from a read-only root directory into writable `/var/lib`; this is an integration result, not a blanket syscall pass.

The installed rootfs still persists `/run` on the host. A stale regular file at `/run/systemd/io.systemd.JournalAccess` caused `systemd-journalctl.socket` to fail in that boot, and old `/run/log/journal` content remains. The next filesystem milestone is a private, empty `/run` per boot, created after the single-rootfs boot lock and mapped through every guest path, symlink, dirfd, and reverse-path operation. Cleanup must wait until every guest process releases its boot lease. Deleting the shared rootfs `/run` while the current GUI is live is unsafe.

## Cases requiring semantic work

- **Mount namespace:** `mount`, `umount2`, `fsmount`, `move_mount`, `fsconfig`, and `mount_setattr` currently report success without creating a mount or updating a coherent mount table. This directly causes systemd's `tmp.mount` to fail its post-mount check. A host-backed `/tmp` plus a documented mask of `tmp.mount` is a safe short-term boot accommodation; actual compatibility requires private mount state, path resolution, `mountinfo`, and `statfs` consistency.
- **Namespaces and security:** `pivot_root`, `chroot`, `unshare`, and `setns` are refused. `seccomp`, `bpf`, Linux Landlock, LSM and new mount API semantics cannot be represented by a case-label success value. If offered to applications, they need explicit translation or a safe failure.
- **Concurrency and modern I/O:** io_uring, POSIX timers, `clone3` variants, `rseq`, `futex_waitv`, and newer futex operations are absent, partial, or return `-ENOSYS`. `epoll_pwait2` now handles unmasked waits with nanosecond timeouts, but a supplied signal mask is explicitly unsupported. Fallbacks may allow a particular program to run, but the ABI itself is not complete.
- **IPC and file features:** SysV IPC has partial handlers and explicit unsupported control modes; keyrings and several file APIs are unsupported. `inotify` and `fanotify` require event/lifetime testing, not an open-call-only test.
- **Placeholder successes:** `syslog`, I/O priority, mount operations, some scheduler policy calls and advisory calls may return success without their Linux side effect. A false success is worse than a clear unsupported return when a caller makes a later decision based on state.
- **Devices and network:** netlink routing/udev, DRM, evdev, ALSA, TTY and GPU paths are synthetic device ABIs. A syscall can succeed while the device contract remains incomplete; test the whole application protocol.

## Verification order

1. **Capture a baseline** using the existing syscall census around a private rootfs boot, login, D-Bus, udev, Hyprland and a representative GUI program. Record syscall number, arguments class, return/errno and caller; avoid logging secrets or full pointer contents. Compare missing calls with the static list.
2. **Create per-call ABI tests** in a disposable rootfs for argument validation, structure layouts, flags, errno, descriptor lifetime, inheritance across fork/exec, polling, concurrency and cancellation. Run equivalent tests on a real Linux ARM64 host as an oracle where practical.
3. **Test interactions**: mount plus `/proc/self/mountinfo` and `statfs`; udev netlink plus socket activation and logind; TTY plus session/controlling-terminal setup; pidfd plus `waitid`/signals; memory mappings plus W^X and JIT.
4. **Track a conformance matrix** with four states per syscall: verified compatible for a stated subset, deliberately unsupported with Linux-compatible error, placeholder/partial, untested. A passing application boot is an integration result, not proof of all syscall semantics.
5. **Prioritize by observed guest demand** while still retaining the full 327-number ledger. Start with boot and desktop blockers, then common application APIs, then privileged or exotic kernel facilities. Regenerate the inventory when the guest kernel headers advance.

Literal full Linux syscall compatibility, including kernel namespaces, security policy, mount isolation and driver interfaces, is a substantial kernel-emulation project. Some semantics cannot be truthfully implemented by returning success on macOS host calls; each needs stateful translation or an explicit unsupported result until implemented.
