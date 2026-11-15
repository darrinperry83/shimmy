# Development source sync — 2026-11-15

This branch now contains the curated source from the advanced `shim-metal-clear`
stage: syscall and systemd boot work, console/VT/input, DRM display changes,
Mac surface bridge, texture bridge, and VideoToolbox host bridge. The build uses
the repository's vendored dependencies instead of prebuilt objects from sibling
workspaces. The texture store and packet source were added to `third_party`.

The development branch's existing Arch bootstrap installer remains `install.sh`.
Stage-specific audio and Omarchy setup is `install-guest-extras.sh`; its optional
Quickshell and Omarchy assets still refer to sibling development directories and
are not included in this source sync. `run.sh --init` provides the systemd path.

The generated Arch rootfs, private boot fixture, logs, binaries, and local
credentials are intentionally absent from Git. `tests/run-login-boot.py` accepts
`RD_LOGIN_ROOT=/absolute/path/to/an/isolated/rootfs` for testing an existing
private fixture without adding it to the repository.

Verification on this Mac: `./build.sh` completed all targets; an isolated
normal `/sbin/init --system` boot using the Git `shim_cli` reached a fresh
`localhost login:` prompt and retained two D-Bus broker processes through a
15-second settle; the standalone socket ABI test passed 179/179; and the VT
exec test passed. These checks do not prove interactive login, installed rootfs
boot, GUI desktop operation, multi-display hotplug, or packaged installation.
