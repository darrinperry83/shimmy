# Boot-to-init (systemd) — `./run.sh --init`

The shim now has a **real Linux boot** path that execs the rootfs's `/sbin/init`
(systemd) instead of a hardcoded guest command (`Hyprland --config ...` for Omarchy,
`/usr/local/bin/rdmetal-audio-boot.sh` for the audio stack). The login → desktop choice
belongs to the guest, not to `run.sh`.

## Usage

```sh
./run.sh --init          # boot systemd; shim_gui shows the VT console (getty → login)
./run.sh --init --cli    # headless variant (shim_cli; no Metal window)
./run.sh --init --gui    # explicit GUI variant
```

`--init` defaults to `shim_gui` because a visible login prompt needs the Metal VT-console
renderer; pass `--cli` for headless runs/tests.

## What it does (file:line)

- `run.sh:37-46` — parses `--init` (plus `--gui`/`--cli`).
- `run.sh:53-80` — the boot-to-init branch:
  - resolves `$ROOT/sbin/init` and fails loudly if it is missing, with the pacman
    incantation to install it (`run.sh:66-73`);
  - `RDUID0=1 exec "$SHIM" "$ROOT" /sbin/init "$@"` (`run.sh:79`) — systemd is exec'd as
    guest argv[0]=`/sbin/init`, exactly like the kernel's `init=` path.
- The shim's exec/ELF-load path is unchanged and generic: `shim.c:1736` (`main`,
  `<rootfs> <guest-path>`) resolves `/sbin/init` through `guest_host_path` → `syscalls_resolve`
  (which follows `sbin -> usr/bin` and `init -> ../lib/systemd/systemd`) and
  `elf_load`s it (`shim.c:1950-1953`).

## Rootfs state (Arch Linux ARM aarch64)

- `systemd 261.2-1` + `systemd-sysvcompat 261.2-1` installed.
- `/sbin/init -> ../lib/systemd/systemd` (realpath `.../rootfs-arch/usr/lib/systemd/systemd`,
  aarch64 ELF, interpreter `/lib/ld-linux-aarch64.so.1` present).
- `getty@.service` present; `getty@tty1.service` enabled; `dbus.service`/`dbus.socket`,
  `sshd.service`, `systemd-networkd.service`, `systemd-resolved.service` enabled.
- `default.target -> graphical.target` (in `/usr/lib/systemd/system`).
- `root` and `alarm` users present (`/usr/bin/bash`).
- `/etc/machine-id` is **empty** — systemd initializes a transient one at boot, but a
  persistent id should be written for dbus/sshd to be stable.

## Scanout independence (RDGPU_SCANOUT=1)

The scanout takeover is a **host-side flag**, not tied to any desktop:

- `src/wlsrv.c:134-141` — `scanout_takeover()` reads `RDGPU_SCANOUT`.
- `src/rdgpu_scanout_client.m:52-58` — `rdgpu_scanout_enabled()` gate (everything off unless set).
- `src/window.m:254-263` — `flick_init()` seeds `gFlickTakeover` from `rdgpu_scanout_enabled()`.
- `src/drmasahi.c:729-775` — `drm_present_fb()` fires on **every** guest DRM page flip,
  whichever DRM-master compositor produced it (Hyprland/aquamarine, KWin, Sway/wlroots).
- `src/window.m:2386-2400` — `gpuwin_present_crtc()` (the drm_present_fb hook) checks
  `gpuwin_takeover_active()` and routes the flipped framebuffer to `rdgpu_scanout_present`.

`run.sh` never unsets or overrides `RDGPU_SCANOUT`; it is inherited verbatim through
`exec "$SHIM"`. So `RDGPU_SCANOUT=1 ./run.sh --init` works for any desktop the user logs
into, independent of the guest init system or DE.

## Validation performed (headless)

1. `./run.sh --init --cli` resolves `/sbin/init -> usr/lib/systemd/systemd`, prints the
   boot-to-init line, and the shim loads and runs systemd:
   `[shim] loaded /sbin/init base=... entry=... interp='/lib/ld-linux-aarch64.so.1'`.
2. Missing-init negative test (`ROOTFS=<empty dir> ./run.sh --init --cli`) prints
   `error: no bootable init at .../sbin/init` + the install hint, exit 1.

## Native rootfs test still pending

The earlier PID-1 check blocker has been resolved in the isolated candidate.
The installed rootfs and graphical `./run.sh --init` path have not been tested
with these candidate changes. Next, verify interactive login and desktop launch
in a separate session before treating this as a usable installed boot.

## Isolated normal-boot result (2026-11-15)

`python3 tests/run-login-boot.py` boots the private Arch fixture with
`/sbin/init --system` and captures a fresh `localhost login:` prompt. The
fixture now uses its original `group: files [SUCCESS=merge] systemd` NSS rule;
the temporary local-only group workaround has been removed. The test deletes
the old console dump before each run and terminates only its own candidate
processes. `RD_LOGIN_SETTLE_SECONDS=15` held the boot for 15 seconds after the
prompt; two D-Bus broker processes remained running. The journal recorded
`Started D-Bus System Message Bus` and `Finished Record System Boot/Shutdown
in UTMP`, with no broker restart or failure during that settled run.

The main fixes that made this possible:

- `/proc/self/status` now reports `CapBnd: 0`, matching `capget` and preventing
  systemd from waiting on unavailable kernel modules.
- Current `PIDFD_GET_INFO` and pidfd registry propagation through fd duplication
  allow `systemd-userdbd` to watch its workers. This clears the `libnss_systemd`
  group lookup wait that had blocked `systemd-tmpfiles-setup-dev`.
- The Linux socket queries `SO_ACCEPTCONN`, `SO_PROTOCOL`, and `SO_DOMAIN` now
  answer for inherited activation sockets, allowing `dbus-broker-launch` to
  accept the bus socket.
- `capget` now negotiates Linux capability ABI v3 on a null data request;
  libcap-ng had treated the old bare success as an uninitialized capability
  state, causing the D-Bus broker child to fail permission drop.
- Unsupported `NETLINK_AUDIT` now fails at socket creation, so optional audit
  disables cleanly. The UTMP boot unit then completes.

This proves a login milestone, not a clean desktop boot. The journal still
shows other unit failures, including network resolution. Interactive login is
tested below; GUI boot remains unverified. Only the isolated fixture and test
binary were used; the installed rootfs was not modified.

## Isolated interactive terminal result (2026-11-15)

`RD_LOGIN_ROOT=<private build/ clone> RD_TEST_JOB_CONTROL=1 python3
tests/run-interactive-login.py` boots systemd, logs in as a throwaway `shimtest`
account, and reaches Bash. `id -un` and `id -u` return `shimtest` and `22001`.
The test then starts `sleep 20` in the foreground, interrupts it with Ctrl-C,
starts another `sleep 20`, suspends it with Ctrl-Z, observes `Stopped` in
`jobs`, and runs a following shell command. Both complete well before the
20-second sleep timeout. The test sets core dumps to zero and uses a private
root and input FIFO; it does not use personal credentials or the live session.

Darwin refused `TIOCSCTTY` after `/bin/login` started a new session, leaving
Bash without job control. Shimmy now tracks session ownership and foreground
process group for its virtual consoles across guest processes. This applies
only to Shimmy-owned VT slaves; ordinary ptys continue using host tty calls.
Console Ctrl-C and Ctrl-Z target the registered foreground guest process group.
This validates the tested Bash interaction on the isolated headless boot; GUI
input and a complete desktop session remain unverified.
