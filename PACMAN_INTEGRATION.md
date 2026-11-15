# Arch package-manager integration check

Tested on 2026-11-15 with a private copy of the Arch Linux ARM root under
`build/pacman-test-root`. The installed rootfs and live Shimmy session were not
modified. Guest processes ran with `RLIMIT_CORE=0`.

## Verified

- Pacman read its existing installed-package database (`pacman -Qi pacman`) and
  repository metadata (`pacman -Si tree`, `pacman -Sp tree`).
- Guest `getent ahostsv4` resolved the official Arch Linux ARM mirror. Guest
  `curl` reached its HTTP endpoint and downloaded the 37,000-byte `tree`
  package directly from the resolved mirror endpoint into the private root.
  The package SHA-256 matched `%SHA256SUM%` in the clone's `extra.db`:
  `e20cb58900b735f3cc79f87c60609e9cdf267fb7139725c9f3816c60efa07856`.
- `pacman -U --noconfirm` installed `tree 2.1.0-1` and completed the
  `ConditionNeedsUpdate` hook. `pacman -Q tree` and `tree --version` worked.
  `pacman -R --noconfirm tree` removed it; both the binary and local database
  entry were absent afterward. A cached `libmpc` reinstall also completed.

The fixes were a guest-visible `/proc/self/mounts` for `/etc/mtab` and absolute
host paths for Shimmy's executable and root across guest `chdir` and re-exec.
Without the first, pacman's disk-space check failed. Without the second,
libalpm's post-transaction shell hook failed with `ENOENT` when it re-execed
the shim from a changed working directory.

## Remaining gaps

- `pacman -Sy` is **not** working. After ownership metadata was made writable
  in the private clone, its download worker stopped at `Landlock is not
  supported by the kernel` / `switching to sandbox user 'alpm' failed`.
  No sandbox or pacman security setting was disabled to make the test pass.
- Guest curl's ordinary asynchronous DNS path initially aborted in glibc's
  `allocatestack.c:335`. Shimmy was passing the *lowest* byte from
  `clone3.stack` as the initial SP and ignoring `clone3.stack_size`. Linux's
  aarch64 child SP is the top of that downward-growing region. After correcting
  the translation, guest curl resolved both mirror hosts itself, followed the
  HTTP redirect, and downloaded the same 37,000-byte package with the matching
  SHA-256. No manual IP override was needed. This fixes the tested curl path;
  other threaded guest programs still need their own regression checks.
- The clone's preexisting pacman configuration uses `SigLevel = Never` for
  repositories. This test did not change it. Matching the package hash to
  `extra.db` checks consistency with that database, but does not establish a
  signed chain of trust.

## Isolated signature verification test

In a separate clone on 2026-11-15, `pacman-key --init` generated a test-root
master key and `pacman-key --populate archlinuxarm` imported the bundled Arch
Linux ARM keys. The clone's general and local-file signature settings were
changed to `Required TrustedOnly`; the live root and repository configuration
were not changed.

Under those mandatory settings, guest pacman installed a locally signed
throwaway package. The same package with a changed byte and its original
detached signature was rejected as `invalid or corrupted package (PGP
signature)`. An unsigned copy was rejected as `package missing required
signature`. The previously installed file remained unchanged after both
rejections.

An official `tree 2.1.0-1` package and its detached signature were downloaded
from an Arch Linux ARM mirror. Guest `pacman-key --verify` reported a good
signature from `Arch Linux ARM Build System <builder@archlinuxarm.org>` with
full trust in the clone's populated keyring. Guest pacman installed the signed
package with `-Udd`, while signature checks remained mandatory. The double
dependency bypass was needed because this particular source root lacks a
pacman local package database; it does not prove normal dependency resolution.
The package SHA-256 matched the earlier `extra.db` value:
`e20cb58900b735f3cc79f87c60609e9cdf267fb7139725c9f3816c60efa07856`.

The tool's own execution sandbox initially blocked the test GnuPG agent's
Unix socket bind. `pacman-key --init` still exited zero despite GnuPG reporting
that no master key was created. A native socket bind to the same temporary
path failed under that tool sandbox and succeeded outside it; rerunning only
the isolated clone test there generated the key. This was a test-harness
restriction, not evidence of a Shimmy socket defect. Future keyring checks
must inspect key presence and GnuPG output, not `pacman-key --init`'s exit
status alone.

These tests demonstrate that Shimmy can run GnuPG and libalpm's local package
signature checks. They do not authenticate the origin of the pre-bundled
keyring or turn the live root's `SigLevel = Never` into a trusted configuration.
Repository sync still stops at Landlock; see
[LANDLOCK_COMPATIBILITY.md](LANDLOCK_COMPATIBILITY.md).

## Whole-process confinement experiment

On 2026-11-15 a separate disposable Arch clone ran under the default-deny
host policy described in `LANDLOCK_COMPATIBILITY.md`. Only the clone's root and
its private socket namespace were writable. The launched process received a
small, credential-free environment and no inherited private file descriptors.
The clone's package and local-file signatures were `Required TrustedOnly`; its
Landlock and seccomp disable flags remained commented out. The original root
and live Shimmy session were untouched.

With pacman's documented external `XferCommand` set to guest curl **in the
clone only**, `pacman -Udd --noconfirm` fetched the official `tree 2.1.0-1`
URL and its detached `.sig` into the clone's package cache. Pacman reported
`checking keyring` and `checking package integrity`, installed the package,
and completed the `ConditionNeedsUpdate` hook. `pacman -Q tree` and
`tree --version` then worked. Confined `pacman -R --noconfirm tree` removed its
binary and local database record and completed the hook. `-dd` bypassed
dependency resolution because this test root had no local pacman database; it
did not bypass signature verification. A separate confined guest curl download
matched the package SHA-256 above, and `pacman-key --verify` reported a good,
fully trusted Arch Linux ARM Build System signature against the clone's
populated keyring.

Two host-path fixes made the confined install repeatable without broadening
the host policy: virtual `/proc` temporary files now honor the existing
`RDX_MEMFD_DIR`, and the shared child registry uses `RDSOCKETGROUP` when one
is supplied. The confined run succeeded with those two host-private paths and
without setting guest `TMPDIR`. A separate negative probe allowed a write
inside the disposable root but denied a write to a throwaway file outside it.

**Repository sync remains blocked.** With pacman's internal downloader,
`pacman -U URL` failed at the honest Landlock check. The external downloader
allowed URL installation, but `pacman -Sy` with `SigLevel = Required
TrustedOnly` stopped because the official mirror returned HTTP 404 for
`core.db.sig` and `extra.db.sig`. The databases themselves returned HTTP 200.
An attempted isolated retry with `DatabaseOptional` was rejected by automatic
approval review *before execution* because it changes the database signature
policy. No equivalent retry or workaround was run, and the clone's policy was
restored to `Required TrustedOnly`. This leaves normal signed repository sync
and dependency resolution unproven.

The host policy is experimental. A deliberately inherited host file descriptor
can bypass pathname denial; the test launcher closes such descriptors, but a
production launcher and its child lifecycle need review. The external curl
path is not pacman's Landlock sandbox, and package hooks and host service
permissions have only been exercised for this small package. No production
fallback has been enabled.

Landlock is a kernel-enforced, inherited restriction on the calling thread's
filesystem access. Pacman creates rules, adds allowed paths, then restricts its
download worker. Returning success for these syscalls without an equivalent
host-enforced policy would falsely report a sandbox while allowing ordinary
host-user filesystem access. Shimmy therefore leaves the syscall unsupported;
`pacman -Sy` was retested after the curl fix and still reports the Landlock
error. A future implementation needs a defined policy translation and negative
tests proving that denied reads and writes really fail across child processes.

The result proves a real, isolated network download and local pacman
install/removal. It does not establish unattended `pacman -S` operation or a
production-ready package-manager security model. See [Linux's `clone(2)`
contract](https://man7.org/linux/man-pages/man2/clone.2.html),
[Landlock semantics](https://man7.org/linux/man-pages/man7/landlock.7.html),
and [pacman's sandbox settings](https://man.archlinux.org/man/pacman.conf.5.en).
