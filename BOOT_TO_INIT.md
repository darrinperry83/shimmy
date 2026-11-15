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

`RD_LOGIN_ROOT=build/<private-arch-clone> python3 tests/run-login-boot.py` boots the private Arch fixture with
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

The isolated boot runners now require a private clone beneath `build/` and
match both the absolute Shimmy executable path and absolute clone path when
counting or stopping child processes. A previous relative-path match missed
re-exec'd guest services, reported zero brokers, and left nine test services
running after a prompt capture. A fresh test captured `localhost login:`,
observed both `dbus-broker` and `dbus-broker-launch` after a five-second
settle, and left zero processes for that disposable root. The interactive
throwaway-account test also reached Bash, passed Ctrl-C/Ctrl-Z job control,
and left zero disposable processes. These checks do not claim a clean boot:
`systemd-resolved` still fails. Its standalone guest trace shows requests for
`SOL_NETLINK` options 11, 12 and 3 returning `ENOPROTOOPT`; Shimmy does not
implement those netlink options, so the service failure remains honest.

The first netlink audit found a more fundamental limit: the old socket path
returned a quiet AF_UNIX datagram for *every* AF_NETLINK protocol. In
[systemd 261's resolver source](https://github.com/systemd/systemd/blob/v261/src/resolve/resolved-manager.c),
`manager_rtnl_listen()` needs `NETLINK_ROUTE` link and address dumps, which
that quiet socket cannot provide. At that stage Shimmy exposed only its existing
`NETLINK_KOBJECT_UEVENT` datagram monitor subset and rejects route, audit,
unknown protocols and unsupported socket types at creation. A focused native
test verifies the monitor's real `SO_RCVBUF` mapping, rejects unsupported
`SOL_NETLINK` options, and rejects the unsupported protocols. CLI and GUI
builds passed. A fresh isolated full boot still reached login with both D-Bus
broker processes after settling, then cleaned up all test processes. The
resolver now reports `Address family not supported by protocol` immediately
instead of failing later on netlink options. **This does not make resolver or
networkd functional.** Real route/link events and dump replies remain needed.

The next step adds a **snapshot encoder**, not a route socket. `src/netlink_route_snapshot.c`
collects the host's actual interface names, indices, flags and MTUs, deduplicates
them by index, and encodes Linux `RTM_NEWLINK` messages with the caller's sequence
number. A deterministic wire-format test plus a live host-interface snapshot
test pass. This is the first read-only piece of the required `RTM_GETLINK` dump.
At that stage `NETLINK_ROUTE` returned `EAFNOSUPPORT`: there was no request/reply socket,
`NLMSG_DONE` completion, `RTM_GETADDR` dump or event stream yet, and exposing
the encoder as a working resolver service would be misleading. Host interface
presence alone also does not prove that an interface is usable inside the guest.

The bounded `NETLINK_ROUTE` transport accepts `SOCK_DGRAM` or `SOCK_RAW`
only with `SOCK_CLOEXEC`. It implements read-only `RTM_GETLINK` and
`RTM_GETADDR` dumps from the host's current interfaces. Link replies carry
actual names, indices, flags, MTUs and link-layer addresses; address replies
carry actual IPv4/IPv6 addresses, prefix lengths and scopes. The exact
`IFLA_EXT_MASK=9` requested by iproute2 is accepted because Shimmy exposes no
virtual functions or statistics; other masks fail explicitly. Multipart replies
preserve the request sequence and local netlink port ID, and finish with a
zero-status `NLMSG_DONE`. Linux's zero-iovec `MSG_PEEK|MSG_TRUNC` length probe is
also supported. Fixed size and count ceilings reject partial dumps; malformed
requests, `RTM_GETROUTE`, multicast subscriptions and unsupported options fail.
The private response descriptor is reserved from guest `close_range`, and
ordinary close and duplication maintain its registry.

Focused wire-format and socket tests pass. On a disposable Arch root, guest
`ip link show`, `ip addr show`, `ip -4 addr show` and `ip -6 addr show` all exit
zero and display the host snapshot. A fresh isolated full boot still reaches
login with D-Bus settled. Address origin and lifetime are not known from
`getifaddrs`; iproute2 may label a non-loopback address `dynamic` without proof
of DHCP. Guest availability of a host interface is also not established by
listing it. `systemd-resolved` still fails with `Protocol not available`:
systemd 261 requires `NETLINK_PKTINFO` during bind and later event subscriptions.
**These are read-only snapshots, not a working resolver or general netlink
implementation.**

The next isolated resolver trace reached `NETLINK_PKTINFO` (option 3), then
`NETLINK_LIST_MEMBERSHIPS` (option 9), then `NETLINK_ADD_MEMBERSHIP` (option 1).
The route socket now stores the packet-info toggle and reports an empty
membership list. Its supported dump replies are unicast and correctly carry
no multicast packet-info control message. Duped descriptors share the toggle.
The resolver still stops at `NETLINK_ADD_MEMBERSHIP`: promising a link/address
subscription without host-backed change notifications would leave it with stale
state. This option remains explicitly unsupported. The trace used a disposable
Arch root; normal boot again reached login with D-Bus settled. A future event
backend must translate actual host link/address changes and preserve group
identity before the resolver can subscribe.

Regression commands: `clang -arch arm64 -O2 -Wall -Wextra -Werror -I src -o
build/test_netlink_route_transport tests/test_netlink_route_transport.c
src/netlink_route_transport.c src/netlink_route_snapshot.c &&
build/test_netlink_route_transport`; `tests/test_netlink_scope.c` exercises the
same path through guest socket syscalls. The isolated boot used a disposable
clone under `build/`; the clone and guest processes were removed afterward.

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
