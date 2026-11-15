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

A further end-to-end probe used an existing Linux root read-only and a
throwaway file in this repository's `build/` directory. The default `/Users`
share let guest `/bin/cat` read the file (exit 0). Running the same Shimmy
command under a host `sandbox-exec` profile denying read access to that one
canonical host path made guest `cat` fail with `Operation not permitted` (exit
1). The throwaway file was removed. This proves the host process policy reaches
the guest's host-shared file access. The profile otherwise allowed access by
default, so it **does not** confine a download worker or justify disabling
pacman's Landlock requirement.

The default rootfs and Shimmy executable also live under `/Users` on this
machine. A broad host deny rule for `/Users` would block the guest root itself
unless an explicit allowlist were proved. Guest `RDSHARE=` can hide shared
paths from Shimmy's path translation, but it cannot isolate native calls in
the shared host address space. Neither is a sufficient replacement for a
host-enforced process boundary.

A narrow allowlist probe denied file reads and writes under `/Users/dperry`,
then allowed read-only access to the Shimmy executable and Linux root plus
metadata traversal for parent directories. Guest `cat /etc/os-release` exited
0, while a guest read of the throwaway file elsewhere in the home tree failed
with `Operation not permitted`.

A stricter default-deny host profile initially aborted even native
`/usr/bin/true` with signal 6 and no output. The macOS sandbox denial log
identified a denied `file-read-data /` operation. Allowing a read of the root
directory itself fixed that native startup failure. Under the corrected
default-deny profile, guest `cat /etc/os-release` succeeded while guest reads
of a host file outside the Linux root failed. A repeatable test in
[`tests/test_host_filesystem_sandbox.py`](tests/test_host_filesystem_sandbox.py)
also confirmed that a guest child could not read that file and a guest could
not overwrite a host file. Its temporary Unix-socket namespace is the only
allowed writable filesystem subtree; the test root is read-only. It passes on
this Mac with both the existing Alpine and Arch test roots when run outside an
enclosing macOS sandbox. An optional guest curl HEAD probe also reached the
official Arch Linux ARM package mirror from the confined Arch process, after
explicitly allowing outbound networking and Unix socket binding in its
private namespace. A later run timed out resolving DNS; the macOS denial log
showed `mach-lookup com.apple.system.opendirectoryd.libinfo` was blocked.
Allowing only that named lookup made two further confined HEAD probes pass.
This checks transport reachability, not a package download or install.

The test denies host operations by default, then permits process execution,
filesystem metadata reads globally, selected system data reads, and optionally
outbound networking plus one OpenDirectory service lookup. Those allowances
require a narrower review before
production use. It is a whole-process policy prototype, not a complete
package-worker profile or
production launcher. It has not been tested with Arch pacman under the
profile, package hooks, all host service calls, other devices or pre-opened
descriptors. It does not implement Linux Landlock semantics.

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
