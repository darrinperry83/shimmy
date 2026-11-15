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

systemd now boots as PID 1 and reaches the **login stage**:
- **journald ACTIVE** ("Started Journal Service").
- **udevd ACTIVE** ("Started Rule-based Manager for Device Events and Files").
- **logind ACTIVE** ("Started User Login Management").
- **"Reached target Basic System" / "Reached target Network" / "Reached target Login Prompts".**
- **"Started Getty on tty1."** — but agetty never actually runs.

The two REMAINING blockers (separate, §5):
- **`getty@tty1` fails to open `/dev/tty1`** → "Failed to set up standard input: No such file
  or directory" → agetty never runs → no `login:` prompt.
- **`systemd-resolved` exits 1** (start-limit-hit), a separate daemon failure.

---

## 5. THE CURRENT BUGS (fix these)

### 5a. `getty@tty1` fails to open `/dev/tty1` (THE remaining login blocker)

agetty never runs because the executor's open of `TTYPath=/dev/tty1` returns **ENOENT** ("Failed
to set up standard input: No such file or directory"). The `[vt] tty1 ready` lines prove the pty
pair IS created, and `vt_open_node()` (`src/vt.c`) returns `-ENOTTY` when the vt master isn't
ready — so the ENOENT is upstream in `resolve_f` (the `/dev/tty1` path resolution in
`src/syscalls.c`), likely a timing/path issue between when the vt is created and when getty opens
`/dev/tty1`. Trace the `openat("/dev/tty1")` resolution (`RDEXECDBG` logs openat paths) and fix it
so agetty's stdin opens the tty and the login prompt appears.

### 5b. `systemd-resolved` exits 1 (separate)

`systemd-resolved.service` exits `status=1/FAILURE` in a start-limit-hit loop. No explicit error
in the journal yet — trace its syscalls to find the failure. (Non-fatal to the login, but should
be fixed for a clean boot.)

### (Already fixed, for reference)

The STREAM-listen accept/epoll bug IS FIXED: `net.c` `sys_epoll_pwait`'s recovery path now
detects a LISTENING STREAM socket from `accept()`'s errno (EAGAIN = listening/empty;
EINVAL/EOPNOTSUPP/ENOTSOCK = not listening), non-blocking-accept()s the pending connection(s)
and re-queues them (`acceptq_push`); `sys_accept4` drains the queue (`acceptq_pop`). Also
`unix_path_to_host` now resolves rootfs symlinks for `connect()` (the userdbd socket `Symlinks=`
aliases). This is what made udevd/logind ACTIVE.

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
- `src/net.c` `sys_epoll_pwait`/`sys_accept4` — STREAM-listening-socket accept re-queue (the
  acceptq_push/acceptq_pop recovery), and `unix_path_to_host` resolves rootfs symlinks for
  `connect()`. This is what made udevd + logind ACTIVE and reached "Login Prompts".
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
