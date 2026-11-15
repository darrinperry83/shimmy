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
- Guest curl's ordinary asynchronous DNS path aborted in glibc's
  `allocatestack.c:335`; direct use of an IP returned by guest `getent` worked.
  This is a workaround for the test, not a general DNS fix.
- The clone's preexisting pacman configuration uses `SigLevel = Never` for
  repositories. This test did not change it. Matching the package hash to
  `extra.db` checks consistency with that database, but does not establish a
  signed chain of trust.

The result proves a real, isolated network download and local pacman
install/removal. It does not establish unattended `pacman -S` operation or a
production-ready package-manager security model.
