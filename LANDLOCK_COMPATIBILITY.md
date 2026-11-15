# Landlock compatibility boundary for package downloads

Status: design investigation, 2026-11-15. No Landlock emulation or pacman
configuration change is implemented here.

## What the current failure means

`pacman -Sy` starts a download worker that requests a Landlock filesystem
ruleset. Shimmy currently returns `ENOSYS` for Landlock syscalls, so pacman
reports that its filesystem sandbox is unavailable and stops. This is a
truthful failure. Successful `pacman -U` installation from a previously
downloaded package does not establish that repository sync works.

Landlock applies a ruleset to the calling Linux thread and its future
children. The policy restricts selected filesystem actions under allowed
paths; access through file descriptors opened before restriction needs special
care. Pacman's documented `DisableSandboxFilesystem` setting permits operation
on kernels without Landlock, but it removes this protection. Shimmy's virtual
guest UID is not a host security boundary, and its default `/Users` and
`/Volumes` shares make an unconfined download worker consequential.

## Host sandbox probe

On macOS, a temporary process-local `sandbox-exec` profile denied read access
to `/private/tmp/shimmy-seatbelt-check/deny` and write access to
`/private/tmp/shimmy-seatbelt-check/write`. A child `/bin/sh` was also denied
the read. All three commands returned exit status 1 with `Operation not
permitted`. The profile used canonical `/private/tmp` paths because `/tmp` is
an alias on this host. These were throwaway files; the profile did not touch
the running Shimmy session or host security settings.

This establishes only that a separate macOS process can be restricted. It is
not a Landlock compatibility test. Shimmy runs the guest thread, Mach exception
loop and other support threads in one host process (`src/shim.c`). Applying a
process-wide host sandbox when one guest thread calls `landlock_restrict_self`
would also affect unrelated support threads. It cannot honestly return success
without a verified mapping for the requested rights, path hierarchy, rule
stacking, inherited children and already-open descriptors.

## Safe engineering path

1. Keep Landlock unsupported until its requested policy is enforced. Do not
   turn `landlock_create_ruleset`, `landlock_add_rule`, or
   `landlock_restrict_self` into success-shaped stubs.
2. First build a host-side isolation boundary for a whole Shimmy guest process
   or a separate download worker. Explicitly define allowed rootfs paths,
   network access, shared directories, child inheritance and host service
   access. Test denied reads, writes, rename/link actions and access through
   pre-opened descriptors. The isolated Arch root used for tests is not itself
   a security boundary.
3. Only after that boundary is demonstrated, evaluate pacman's supported
   fallback settings inside the confined guest. Record the loss of Landlock
   and seccomp separately; Shimmy also currently returns `ENOSYS` for seccomp.
   Do not describe the fallback as equivalent to pacman's Linux sandbox.
4. Restore package signature verification before treating repository sync as
   trusted. The existing test root's `SigLevel = Never` means matching a
   package hash to its repository database is only a consistency check.

For now, the verified path is an ordinary guest network download followed by
an isolated `pacman -U` install/removal. `pacman -Sy` remains blocked.

References: [Landlock overview](https://man7.org/linux/man-pages/man7/landlock.7.html),
[Landlock ruleset API](https://man7.org/linux/man-pages/man2/landlock_create_ruleset.2.html),
[pacman.conf sandbox settings](https://man.archlinux.org/man/pacman.conf.5.en),
[Apple App Sandbox](https://developer.apple.com/documentation/security/app-sandbox).
