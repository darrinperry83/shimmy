# Shimmy — handover for continuation (boot-to-init)

This is a self-contained handover for continuing a specific task: **getting `./run.sh --init`
to reach a real Linux login prompt** by booting systemd (PID 1) inside the shim. It assumes no
prior context. Read this fully before acting.

---

## 1. The goal (one paragraph)

The shim runs an Arch aarch64 Linux rootfs as a guest process on macOS, translating Linux
syscalls to the host (Darwin/Mach). The `--init` path boots `/sbin/init` (systemd 261) as PID 1,
so the user gets a normal Linux boot: systemd → getty → login → any desktop. We are ONE bug away
from the login prompt. The task is to fix that bug and land the login prompt.

---

## 2. Where everything lives

- **Shim source (the ONLY tree you edit):**
  `/Users/dperry/Documents/Codex/2026-11-07/task/hyprland-stage/metal-wayland-next/shim-metal-clear`
  - `src/syscalls.c` — the main Linux syscall dispatcher + `/proc` `/sys` `/dev` synthesis + cgroup synthesis.
  - `src/syscalls_ext.c` — the fallback syscall tier.
  - `src/net.c` — the socket/epoll/kevent emulation (THE FILE FOR THE CURRENT BUG).
  - `src/shim.c` — the loader, exec/fork/clone, env.
  - `src/timerfd_shim.c`, `src/pidfd_shim.c`, `src/signalfd_shim.c` — the fd shims.
  - `src/gmem.c`, `src/vt.c`, `src/console.c` — memory, VT console, terminal emulation.
  - `build.sh` — `./build.sh cli` (the headless runner `shim_cli`), `./build.sh gui`, `./build.sh windowserver`.
  - `run.sh` — `./run.sh --init` boots systemd; `./run.sh --init --cli` is the headless variant.
- **Rootfs (guest):** `/Users/dperry/Documents/Codex/2026-11-07/task/hyprland-stage/metal-wayland-next/rootfs-arch`
  (Arch aarch64, systemd 261.2-1, Hyprland, the DeepSeek Harness + Codex + toolchain).
- **Repo (docs/config, git-tracked):** `/Users/dperry/rmatrixos-shim` (github `darrinperry83/shimmy`, branch `development`).

**⚠️ DO NOT MODIFY `/Users/dperry/linux-shim`** — it is a historical reference tree; all work is in `shim-metal-clear/src` (now self-contained, no symlinks).

---

## 3. The build/run commands

```sh
cd /Users/dperry/Documents/Codex/2026-11-07/task/hyprland-stage/metal-wayland-next/shim-metal-clear
./build.sh cli                          # rebuild shim_cli

# headless boot (systemd as PID 1):
RDUID0=1 ./shim_cli ../rootfs-arch /sbin/init --system

# with the debug traces that are already wired in:
RDUID0=1 RDNETSOCK=1 RDERRDBG=1 RDNOTIFYDBG=1 RDEXECDBG=1 \
  ./shim_cli ../rootfs-arch /sbin/init --system
```

The boot progress is in `/tmp/rdshim-kmsg-*.log` (the synthesized `/dev/kmsg`). The console
dump is via `RDCONS_DUMPFILE` (a path; set it to capture the tty1 login state).

---

## 4. Current state (exactly where we are)

systemd now boots as PID 1 and gets VERY far:
- **journald is ACTIVE** ("Started Journal Service").
- **udev coldplug finishes** ("Finished Coldplug All udev Devices").
- **"Created slice /system/getty"** appears.
- The syscall surface is broadly working: PID-1 identity, cgroup v2, timerfd, pidfd, socket
  activation, SCM_CREDENTIALS (sd_notify), the `/proc`+`/sys`+`/dev` synthesis.

The boot is **blocked at two units that never go ACTIVE**, and both share ONE root cause
(§5):
- `systemd-userdbd.service` never starts (its socket activation never fires).
- `systemd-udevd.service` stalls in rule-parsing (its `getgrnam` NSS lookup talks varlink to
  the never-started userdbd, so it times out at 90s).
- `systemd-update-utmp` also hangs on the journal stdout stream.

---

## 5. THE CURRENT BUG (fix this)

**A LISTENING AF_UNIX SOCK_STREAM socket with a pending connection does NOT wake the owner's
`epoll_wait`, so `accept()` never runs.**

Measured facts (from `RDNETSOCK=1`, log `/tmp/rd_netsock_dbg.txt`):
- A **SOCK_DGRAM** socket (e.g. fd 32, journal/socket) DOES fire `kevent` (EVFILT_READ).
- A **SOCK_STREAM LISTENING** socket (e.g. PID 1's `/run/systemd/userdb/io.systemd.Multiplexer`
  on fd 46) with a client `connect()` pending does **NOT** fire `kevent` — `EP_APPLY` registers
  `EPOLLIN` (events=0x1) into the kqueue AFTER `listen`, the client connect succeeds, but no
  `KEVENT fd=46` ever appears.
- So the guest's `epoll_wait` never returns EPOLLIN for the listening socket, `accept()` is never
  called, and the daemon behind it (userdbd, and journald's stdout stream) never starts.

This is the SAME bug class the code already works around for DATAGRAM sockets. Look in
`src/net.c` at `sys_epoll_pwait` / `ep_apply` / the "epoll recovery" block (which, for datagrams,
checks `MSG_PEEK>0` and re-applies the kqueue registration + hands back EPOLLIN).

### The fix (recommended)

Extend that recovery to **STREAM LISTENING** sockets:

1. Detect a listening socket via `getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, ...)`.
2. For a listening socket that is absent from the `kevent()` result, prove a connection is
   pending. `SO_NREAD` returns 0 for listen sockets, so the reliable proof is a **non-blocking
   `accept()`**: set `O_NONBLOCK`, `accept()` one connection, then **re-queue it** (the accepted
   socket must be handed back so the guest's own `accept()` succeeds — e.g. keep it and serve it
   from the shim's fd table, or find the socket option that suppresses EVFILT_READ on listen
   sockets and drop it).
3. Hand back `EPOLLIN` so the guest's `epoll_wait` wakes and it calls `accept()`.

Alternative (cleaner, if findable): identify WHY the shim's setup suppresses `EVFILT_READ` for
STREAM listen sockets (a `SOCK_CLOEXEC`/`SOCK_NONBLOCK` application, `SO_NOSIGPIPE`,
`kill_sigpipe`, or a wrong `listen()` emulation) and drop it. Note a plain host test
(bind/listen/EVFILT_READ/kevent + fork/connect) DOES fire for STREAM, so it is NOT a raw-kqueue
limitation — it is something the shim does to STREAM listening sockets specifically.

### Secondary (after the main fix)

- `systemd-userdbd.socket` has `Symlinks=` (`io.systemd.DropIn`, `io.systemd.NameServiceSwitch`)
  that the shim creates only in the ROOTFS, but socket `connect()` maps paths into the per-boot
  group prefix — so nss-systemd's `connect()` to those aliases returns ENOENT. Create the symlink
  aliases in the group namespace too (or make `unix_path_to_host` resolve the rootfs symlink).
- Optionally add the missing groups (`i2c`, `clock`, …) to the rootfs `/etc/group` to silence the
  `Failed to resolve group '...'` udev warnings and remove the NSS round-trip.

---

## 6. Fixes ALREADY landed (do NOT redo these — they are in the current source)

All in `shim-metal-clear/src` (build clean):

- `src/syscalls.c:465` + `:4028` — map macOS `/private/` → `/` (the `/tmp` → `/private/tmp`
  canonical path caused `systemd`'s CREDENTIALS step to ENOENT; fixed journald's "Failed at step
  CREDENTIALS").
- `src/syscalls_ext.c:1680` — `fsopen`/`fspick` return a `O_NONBLOCK` socket (the `tmp.mount`
  helper hung forever in `read()` on a blocking unconnected DGRAM; `read()` now returns EAGAIN).
- `src/net.c:527-535` — hash long abstract unix-socket names to a 16-hex FNV-1a token (userdbd's
  ~44-byte abstract name hex-encoded to ~88 bytes overflowed macOS's 104-byte `sun_path` →
  `ENAMETOOLONG`).
- Earlier (already in): PID-1 `getpid()==1` (`src/syscalls.c:344` `syscalls_guest_pid`), cgroup
  v2 synthesis, timerfd `TIME_T_MAX` saturation, pidfd registry (fd never recycled), inotify
  write-end reserved, `pack_fds` (shim fds relocated to `>= 256`), SCM_CREDENTIALS via
  `.rdx-sender/<pid>.<seq>` autobind, the group/uid syscall fakes under `RDUID0`, `fchmodat2`
  AT_EMPTY_PATH, `faccessat2` AT_EMPTY_PATH, `/dev/kmsg` tail-f pipe.

---

## 7. Debug knobs (already wired, env-gated)

| Env | What it logs |
|---|---|
| `RDNETSOCK=1` | bind/listen/connect/accept/EP_APPLY/KEVENT with fd numbers → `/tmp/rd_netsock_dbg.txt` |
| `RDERRDBG=1` | any syscall returning `-17`/`-22`/`-36` + fd + guest path |
| `RDNOTIFYDBG=1` | sd_notify datagram + SCM_CREDENTIALS → `/tmp/rd_notify_dbg.txt` |
| `RDEXECDBG=1` | exec/fork/clone/read/openat + the LISTEN_FDS env at exec → `/tmp/rd_exec_dbg.txt` |
| `RDCONS_DUMPFILE=<path>` | dump the tty1 console to `<path>` |

**Gotcha:** `RDFUTEX=<prefix>` is BROKEN as a diagnostic — it keeps the trace file open, eats a
low fd, and shifts the guest fd table. Avoid it.

---

## 8. What "done" looks like

The milestone is the login prompt. Run:

```sh
RDUID0=1 RDCONS_DUMPFILE=/tmp/cons.txt ./shim_cli ../rootfs-arch /sbin/init --system
```

and confirm `/tmp/cons.txt` (or the kmsg) shows a getty login prompt, e.g.:

```
Arch Linux 6.x (tty1)
login:
```

(or `systemd[1]: Started Getty on tty1.` in the kmsg). Also confirm `systemd-userdbd` and
`systemd-udevd` are ACTIVE (`Started User Database Manager`, `Started Rule-based Manager for
Device Events and Files`).

---

## 9. Do not get lost — the key architectural facts

- The shim translates Linux syscalls to Darwin **in user space** (no kernel extension). syscalls
  are dispatched in `src/syscalls.c` `linux_syscall()` and `src/syscalls_ext.c`
  `linux_syscall_ext()`.
- The guest's `/proc`, `/sys`, `/dev`, and cgroup v2 are **synthesized in memory** (never the
  rootfs). `mem_fd()` (`syscalls.c:907`) serves them as unlinked temp files.
- AF_UNIX sockets are emulated in `net.c` over macOS sockets + a per-boot group prefix. The
  kqueue/kevent ↔ epoll mapping is in `sys_epoll_pwait`/`ep_apply`.
- The guest runs as a normal macOS process (uid 501); `RDUID0=1` fakes root so systemd's setuid/
  setgid/setgroups/capset succeed.

Good luck — it is genuinely one fix from the login prompt.
