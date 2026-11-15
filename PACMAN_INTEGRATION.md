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
