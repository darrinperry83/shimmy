# Full init boot and Arch installer

The acceptance target is original Arch Linux ARM systemd completing a real boot
into a usable installer on the XNU Linux personality. Individual syscall tests,
a callback loop, a tiny service target or an exported package root do not satisfy
that target. They remain regressions and preparation steps.

The immediate installation layout is a directory on the existing macOS Data
volume. The installer root and installed Arch root have separate paths.
XNU binds the selected directory as the Linux domain's root. Preserve
numeric ownership and modes, and use actual XNU credentials/VFS enforcement.
There is a concrete constraint on a plain directory: `check_directory.py` found
64 filename collision candidates in the pinned archive, including ASCII pairs
`usr/include/linux/netfilter/xt_CONNMARK.h` and `xt_connmark.h`, and distinct
xtables libraries. A plain case-insensitive APFS directory cannot preserve this
root. Keeping storage on the existing partition is still possible using a
case-sensitive filesystem image stored there and mounted at the chosen root
directory; a case-sensitive APFS volume in the same container is another option.
Neither storage mechanism is implemented or selected by this preflight. Do not
extract the full archive over a case-insensitive directory and silently lose
files. Reproduce with `python3 experiments/xnu-linux-abi/installer/check_directory.py`.

The usable milestone requires all of: full native systemd unit boot, interactive
console input and job control, working network/DNS/HTTPS, a verified package
transaction into the selected directory, and a subsequent boot of that installed
root. Neither an unpacked root nor active network services alone count. Reuse
the shim's behavioral tests where applicable, while selecting implementations
around XNU's native facilities. Ordinary installed package updates remain pacman
transactions; downloaded root archives serve live installation and recovery.

The first root capture contains 172 packages and 381 packaged systemd unit
files. Its real Linux PID 1 reference reaches the installer target with 114
active units and no failures, including journald, D-Bus, logind, udevd, console
getty, networkd and resolved. The compressed archive is 441,237,402 bytes, pinned
by SHA-256 in [evidence.json](evidence.json). Native XNU full-init, interactive
input, online package installation and target installation gates remain false.

## Installer root

`prepare.py` captures an independently pinned, complete package root, including
the pacman local database, Bash, systemd and arch-install-scripts. It retains the
original package binaries and normal systemd unit definitions. The installer
profile makes `rdxnu-installer.target` the default target and requires the normal
`multi-user.target`, networking daemons and an installer readiness service. The
console getty uses local root autologin for this installer profile. No SSH server
or remotely accessible login is enabled by these changes.

The readiness service requires real PID 1 systemd and active sysinit, basic,
multi-user, console-getty, networkd and resolved services. It checks the package
database, executes Bash and the pacstrap help path, and rejects failed units.
Starting network services alone does not prove DHCP, DNS, HTTPS or package
installation. Interactive console input and disk installation have separate gates.

The root archive preserves numeric ownership, modes, symlinks, ACL/xattr tar
metadata and the package database. Runtime mounts and Docker's injected host
configuration are excluded; empty mountpoints and deterministic hostname/hosts/
resolver configuration are added explicitly. Host home directories, package
cache and runtime logs are excluded. Required file hashes are checked in the
archive against the profile inputs; the capture rejects source changes.

Create the disposable updated container, then capture it:

```sh
docker run --name rdxnu-installer-build-20261127 --network bridge \
  sha256:0151e70bd01e7e4c891e903edf1dacba81656f04c6150bfce78d5a570e4df443 \
  sh -c 'pacman --disable-sandbox -Syu --needed --noconfirm arch-install-scripts'
python3 experiments/xnu-linux-abi/installer/prepare.py \
  --updated-container rdxnu-installer-build-20261127
python3 experiments/xnu-linux-abi/installer/reference.py
```

The pacman sandbox option is confined to the disposable Linux package-build
container, whose current runtime cannot apply pacman's Landlock sandbox.
Package signature checks stay enabled. A full package upgrade avoids a partial
rolling-release upgrade. Existing XNU regression package pins are unchanged.
The new installer image, package inventory and archive are separately hashed in
ignored `out/installer/`. Do not copy an archive into the VM as a bootable image
until the native init/runtime gates pass.

## Boot gates

| Gate | Required evidence |
| --- | --- |
| Complete root | Package database, original executable/library hashes, modes/ownership and installer tools captured in a root archive. |
| Linux reference | Real Linux PID 1 boots the full installer target, required services are active, no failed units, package set unchanged. Container conditions remain visible. |
| Native init lifecycle | XNU owns a Linux init domain with coherent IDs, child waiting, orphan adoption, signals and init-death teardown. See [LINUX-INIT.md](../LINUX-INIT.md). |
| Native runtime root | Actual root/cwd/fd resolution plus truthful proc, runtime, device and required cgroup/mount interfaces. No boot-marker workaround. |
| Native full unit boot | Unchanged systemd reaches the installer target with every required unit active; diagnostics preserve failures and skipped conditions. |
| Interactive installer | Real console input/output, job control, package verification, networking/DNS/HTTPS and pacman transactions. |
| Installation round trip | Install to an explicitly selected disposable target, verify its package database, then reboot into that installed root and reach its target. |

The Linux reference imports the actual hashed root archive into a fresh image
and boots that artifact. It runs the complete normal target graph, rather than replacing
it with the earlier `DefaultDependencies=no` one-service proof. It adds an exit
supervisor only to a separate reference image. That supervisor waits until the
installer target is active, captures all unit states and exits the container.
Those exit actions are absent from the installer archive.

No Linux reference result promotes the native XNU, console-input, networking or
installation gates. Root files are managed on writable storage; the separate
XNU/Apple boot environment remains governed by [UPDATE-POLICY.md](../UPDATE-POLICY.md).
