# shimmy — Handover

> **Last updated:** 2026-11-15 · **Reference machine:** Apple M2 Max, 96 GB MacBook Pro,
> macOS 26.5.1 (25F80) arm64e.
>
> This is the one document to read when taking over the project. It tells you the goal, the
> architecture, exactly where everything lives, what is built (with file:line), what is in
> progress *right now*, the key insights and dead-ends, and the build/run/recovery commands.

---

## 1. The GOAL (north star)

**A real Linux environment on macOS — indistinguishable from a native machine.**

Concretely:

- The **shim is the WindowServer stand-in**. macOS background services (launchd, Dock,
  SystemUIServer, loginwindow) and every AppKit app connect to the shim's ports, never to the real
  WindowServer.
- **mac apps run *inside* Linux.** Each mac app's window surfaces as a native Wayland window in
  the Linux compositor (Hyprland), composited alongside Linux windows into one scene.
- **Boot is a real Linux login.** `./run.sh --init` execs the rootfs's `/sbin/init` (systemd) as
  guest PID 1 → getty → login → whatever desktop the user installed. No hardcoded command, no
  hardcoded DE.
- **NO custom `wlsrv`.** Stock Hyprland/KDE/Sway run unmodified as the DRM master of the shim's
  `/dev/dri/card0`; the shim's *only* remaining rendering-path job is to **present the flip**
  (into a macOS window in windowed mode, or straight to the display via `present_iosurface` in
  takeover/kiosk). The nested-Wayland `wlsrv` is retired — see `DESIGN.md:38-66`.

Linux is the primary experience; **macOS is the hardware layer underneath** (the Mach scheduler +
the real Apple drivers).

---

## 2. The ARCHITECTURE

The shim **borrows the Mach hardware** rather than emulating or virtualizing it. A four-layer
stack (see `DESIGN.md:11-36` and `README.md:99-124`):

```
┌──────────────────────────────────────────────────────────────┐
│  Linux guest (Arch aarch64, glibc)                            │
│  TTY console (getty) → Hyprland / KDE → apps                  │
└──────────────────────────────┬───────────────────────────────┘
                               │ syscalls · Wayland · DRM · /dev/snd
┌──────────────────────────────▼───────────────────────────────┐
│  shim (macOS process, scheduled by Mach)                      │
│  · syscall translation · /dev/ttyN · /dev/dri · /dev/snd      │
│  · input (evdev/libinput from HID) · VT switching             │
│  · Metal present path (CAMetalLayer / present_iosurface)      │
└──────────────────────────────┬───────────────────────────────┘
                               │ IOSurface handoff
┌──────────────────────────────▼───────────────────────────────┐
│  rdmetal (Mesa Gallium driver + DRM/GBM node — we wrote it)   │
│  · lowers guest GL/EGL: NIR → SPIR-V → Metal Shading Language │
│  · presents /dev/dri/renderD128 as the guest GPU              │
└──────────────────────────────┬───────────────────────────────┘
                               │
┌──────────────────────────────▼───────────────────────────────┐
│  scanout (WindowServer INJECTION — we wrote it too)           │
│  · drives present_iosurface → CoreDisplay → IOMFB swap        │
│  · suppresses WindowServer's own compositing on taken displays│
└──────────────────────────────────────────────────────────────┘
```

Every guest subsystem maps onto a real macOS driver (see `docs/HOW_IT_WORKS.md`):

| Guest side | macOS side |
|---|---|
| OpenGL/EGL | Metal (the Apple GPU) |
| frame buffer | IOSurface |
| display flip | IOMFB / the display controller |
| audio | CoreAudio / AudioUnit |
| input | NSEvent / IOHID |

### The WindowServer replacement is via INJECTION

The north-star is that the shim *is* the WindowServer — but **not as a separate boot-time
process**. The settled mechanism is **injection**: the shim's code runs **inside the already
running, already entitled WindowServer** (uid 88), and takes over its compositing + window + input
planes. This is stated at the top of the injection plan:

> *"The injection route is the settled one: the shim's code runs inside the running, entitled
> WindowServer … We do not fight boot-time."*
> — `…/integration/rdmetal/WINDOWSERVER_TAKEOVER_PLAN.md:3-8`

The **~37-function CGS/SLS stand-in** (`shim-metal-clear/src/windowserver.c`) is the same code in
both routes: it runs standalone as `shim_windowserver` (the boot-time fallback) **and** is
`#include`d verbatim into the injection payload (`payload_cgs_standin.m`). Two routes:

- **Route A (implemented, headless-validated)** — embed the stand-in inside the injected process
  and serve it on a Mach port. `WINDOWSERVER_TAKEOVER_PLAN.md:107-113`.
- **Route B (the live takeover, next round)** — patch SkyLight's **server-side MIG dispatch table**
  so the existing `com.apple.windowserver` port answers the ~37 routines with the stand-in's
  handlers. `WINDOWSERVER_TAKEOVER_PLAN.md:115-136`.

---

## 3. Where everything lives

| Location | What it is | Git |
|---|---|---|
| `/Users/dperry/rmatrixos-shim` | **shimmy** — the umbrella/config repo: `bin/shimmy` CLI, `src/chooser.c` boot chooser, `docs/*.md`, `config/*.plist`, `build.sh`, `README.md` | ✅ tracked · branch `development` · remote `https://github.com/darrinperry83/shimmy.git` |
| `/Users/dperry/Documents/Codex/2026-11-07/task/hyprland-stage/metal-wayland-next/shim-metal-clear` | **The SHIM C source** — `src/*.c`, `build.sh`, `run.sh`, `install.sh`, `vaapi/` (media engine), built binaries `shim_cli` / `shim_gui` / `shim_windowserver` | ❌ **NOT git-tracked (local only)** |
| `/Users/dperry/Documents/Codex/2026-11-07/task/linux-shim-integration-next/repo/integration/rdmetal` | **scanout / WindowServer reverse-engineering tree** — `payload_scanout_server*.m`, injectors, `SKYLIGHT_SCANOUT_*.md`, `WINDOWSERVER_*.md`, `CGS_*.md` | ✅ local repo · branch `integration/rdmetal-dmabuf` · remote `origin = /Users/dperry/linux-shim` |
| `/Users/dperry/Documents/Codex/2026-11-07/task/hyprland-stage/metal-wayland-next/rootfs-arch` | **Arch aarch64 rootfs** — systemd 261, Hyprland, DSH + Codex + toolchain | ❌ build artifact (gitignored) |
| `/Users/dperry/linux-shim` | The "mother" source tree (historical reference); `shim-metal-clear/src/*` is now **self-contained** (all symlinks were replaced with real copies) | ⚠️ **DO NOT MODIFY** (read-only) |

### GitHub repos

| Repo | Description (from API) | Role | Default branch |
|---|---|---|---|
| [`darrinperry83/shimmy`](https://github.com/darrinperry83/shimmy) | the umbrella / config repo | this repo | `main` (active work on `development`) |
| [`darrinperry83/shimmy-rdmetal`](https://github.com/darrinperry83/shimmy-rdmetal) | `rdmetal-driver` · language **C** · GPL-3.0 | the Mesa Gallium GPU driver | `main` |
| [`darrinperry83/shimmy-scanout`](https://github.com/darrinperry83/shimmy-scanout) | `rdmetal-scanout` · language **Objective-C** · GPL-3.0 | the WindowServer injection payloads | `main` |

- `shimmy-rdmetal` and `shimmy-scanout` are declared as git **submodules** of `shimmy`
  (`/Users/dperry/rmatrixos-shim/.gitmodules`: paths `rdmetal/` and `scanout/`). They are currently
  **not checked out** (both submodule dirs are empty); the live copies of the same code live in the
  two local trees above.
- The wiki is at [`github.com/darrinperry83/shimmy/wiki`](https://github.com/darrinperry83/shimmy/wiki),
  with a **Recovery** page (the get-back-to-macOS ladder, mirrors `docs/BOOT-ESCAPE.md`).

### Inside `rmatrixos-shim` (the shimmy repo)

- `bin/shimmy` — the CLI (1229-line Python 3, stdlib-only): `config validate|render`, `run`,
  `build`, `recover`, `boot default|once`, `takeover`, `release`.
- `bin/shimmy-installer` — the graphical recovery installer app (Qt6/PySide6).
- `src/chooser.c` — the boot chooser (1516 lines).
- `src/` — 113 files; the *shim* C/ObjC source (mirrors `shim-metal-clear/src`, plus
  `windowserver.c/h`, `secondary_window.m`, `wlsrv.c`, `x11srv.c`, `chooser.c`, etc.).
- `docs/` — 12 markdown design/status docs. `config/` — the chooser plist + config examples.
- `build/` — build artifacts + the WindowServer plist `orig`/`edited`/`override`.

### Inside `shim-metal-clear` (the live shim source)

`src/` holds 133 entries; it is **fully self-contained** — all former symlinks into
`/Users/dperry/linux-shim/src` were replaced with real copies, and the stale `rootfs` symlink was
removed (the real rootfs is `../rootfs-arch`). The live, actively-edited files are:

- `syscalls.c` (205 KB), `syscalls_ext.c` (91 KB), `shim.c` (122 KB), `net.c` (111 KB),
  `gmem.c` (227 KB), `fastcall.c`, `vt.c`, `console.c`, `window.m`, `drmasahi.c`, `snd_alsa.c`,
  `evdev.c`, `windowserver.c` (51 KB), `windowserver.h`, the `rdgpu_*` broker files,
  `timerfd_shim.c`, `pidfd_shim.c`, `signalfd_shim.c`, `rdva_host.m`.
- `vaapi/` — the media-engine driver (`rdva_drv_video.c`, `rdva_probe.c`, `build.sh`, `STATUS.md`).
- Binaries: `shim_cli` (headless), `shim_gui` (Metal window host), `shim_windowserver`
  (stand-in), `ws_test_peer`, `ws_real_client`.

### Inside `integration/rdmetal` (the RE tree)

47 markdown notes + ~100 payload/injector/harness files:

- `payload_scanout_server.m` … `payload_scanout_server43.m` — the scanout payloads (each round
  adds a fix/feature; server43 is current).
- `payload_cgs_standin.m`, `payload_cgs_route_b.m` — the CGS stand-in payloads (Route A/B).
- `payload_flip*.m`, `payload_mac_surface.m`, `payload_readonly*.m` — earlier probes.
- `ws_inject_manual.m` (built as `ws_inject_manual_mac` / `_d` / `_d2`), `inject_stub_manual.s` —
  the arm64e **manual Mach-O bundle injector** (no dlopen; WindowServer's library-validation
  refuses dlopen).
- `SKYLIGHT_SCANOUT_ROUND*.md` (ROUND3 … ROUND37_SERVER42) — the running RE journal.
- `WINDOWSERVER_*.md` (BOOTSTRAP_REGISTRATION, TAKEOVER_PLAN, ROUTE_B, INPUT_PATH, APP_LIFECYCLE).
- `CGS_BACKGROUND_SERVICES_SURFACE.md`, `CGWINDOW_REGISTRY_INVESTIGATION.md`.
- `DESIGN.md`, `SCANOUT_SERVER_RUNBOOK.md`, `SNAPSHOT.json`.
- `broker/`, `carrier/`, `cgs_surface/` (CGS symbol dumps for Dock/NotificationCenter/
  SystemUIServer/loginwindow), `patches/` (`hyprland-0.49-standard-dmabuf.patch`,
  `mesa-24.0.9-rdmetal.patch`).
- Harnesses: `e2e_mach_test.c`, `register_handoff_test.c`, `pu_probe.c`, `vsync_probe.c`,
  `surface_probe*.m`, `dlopen28..43_harness.c`, plus `dasm.py` / `dsc.py` / `dump.py`
  (shared-cache disassembly tooling).

---

## 4. WHAT'S BUILT (with file:line)

### 4.1 The Linux environment

| Subsystem | Status | Where |
|---|---|---|
| Arch aarch64 boot to TTY login | ✅ | rootfs-arch `systemd 261.2-1`; `/sbin/init → ../lib/systemd/systemd` |
| **DRM-master compositor** (Hyprland/KDE run as `MODE_ATOMIC` DRM master) | ✅ | `drmasahi.c:729-775` `drm_present_fb()` fires on every guest DRM page-flip |
| **rdmetal** GPU (Mesa Gallium, NIR→SPIR-V→MSL via spirv-cross; default GL, no env vars) | ✅ (renderer) · 🚧 (live guest still softpipe) | `build.sh:30` CORE list; `rdgpu_guest_syscall.c` / `rdgpu_mock_gate.c` (syscall `0x2002` broker) |
| Audio — PipeWire/ALSA → CoreAudio | ✅ | `snd_alsa.c`; `run.sh` `RDSND=1` |
| Input — evdev/libinput from HID | ✅ | `evdev.c` `evdev_inject`; `window.m:2588` `kVK2EVDEV` |
| TTY / VT console (getty, Ctrl+Alt+Fn) | ✅ | `vt.c`, `console.c` |
| mac apps as Wayland surfaces | ✅ | `secondary_window.m`, `rd_mac_app.m`, `rdgpu_surface_handoff.c` |
| Cross-process dma-buf | ✅ | `drmasahi.c` `PRIME_FD_TO_HANDLE`; `patches/hyprland-0.49-standard-dmabuf.patch` |
| Direct-display takeover (stable `present_iosurface` flip) | ✅ | `rdgpu_scanout_client.m`, `window.m:2386-2400` `gpuwin_present_crtc()` |

### 4.2 DeepSeek Harness + Codex + OpenAI SDK inside Linux

All installed in the rootfs (not host-only):

- `/opt/dsh` — the **DeepSeek Harness** (node_modules incl. `@deepseek-ai/dsh-llm-deepseek`,
  `@deepseek-ai/dsh-web-search-deepseek`).
- `/opt/openai` — the **OpenAI SDK** (`openai-chat.mjs` + `openai` node_modules);
  `/usr/bin/openai-chat` is a 72-byte wrapper into it.
- `/usr/bin/codex` — the **Codex** CLI (288 MB binary) + `/root/.codex` config.
- Toolchain: `gcc`/`g++` (`usr/bin/cc → gcc`), `node v26.9.0`.

### 4.3 The WindowServer stand-in (~37 CGS functions + real SkyLight MIG)

`shim-metal-clear/src/windowserver.c` (1106 lines) + `windowserver.h` (448 lines):

- **Mach bootstrap check-in** — `ws_bootstrap_check_in_system()` (`windowserver.c:213`) does
  `bootstrap_check_in("com.apple.windowserver")` then `("com.apple.windowserver.active")`, gated on
  `RDNATIVE_WINDOWSERVER_SYSTEM=1` + root. The lowercase names are the real service names — the
  uppercase `com.apple.WindowServer` is *only* the launchd Label/sandbox name
  (`WINDOWSERVER_BOOTSTRAP_REGISTRATION.md:1-27`).
- **The routine table** — 38 routines, ids `1000..1036` (`windowserver.c:274-289`):
  `SLSMainConnectionID`, `SLSNewConnection`, `SLSServerPort`, `SLSReleaseConnection`,
  `SLSConnectionGetPIDAndVersion`, `SLSGetConnectionIDForPSN`, the `SLSSession*` family,
  `SLSSetLoginwindowConnection`, `SLSSetLoginName`, connection properties, notify-proc
  registration, `SLSGetEventPort`, the `SLS*Display*` family, `SLSNewWindowWithOpaqueShape(+AndContext)`,
  `SLSOrderWindow`, `SLSSetWindowLevel/Tags`, `SLSReleaseWindow`, `SLSTransactionCreate/Commit`,
  and `SLSBindSurface`.
- **The real SkyLight SLS MIG wire protocol** — real routine ids `0x74xx–0x7dxx`, reply id =
  request + `0x64`, NDR_record at offset 0x18, the `0x80001513`/`0x1513` envelope
  (`windowserver.c:342-365` "REAL SkyLight SLS MIG reply sender" + `:365` dispatch). Real ids
  pinned: `SLSNewConnection 0x7d00`, `CreateWindow 0x7637`, `BindSurface 0x758d`,
  `OrderWindow 0x753a`, `OrderWindowList 0x753b`, `GetWindowList 0x7573`, `ReleaseWindow→TerminateWindow 0x7535`,
  `TransactionCommitSync 0x76de`/`Async 0x76dd`, `GetDisplayForUUID 0x7669`.
- **Validation** — `ws_test_peer` (29 checks, stand-in table) and `ws_real_client` (21 checks,
  real protocol); both 100% pass headlessly (`WINDOWSERVER_TAKEOVER_PLAN.md:201-210`).

### 4.4 The surfacing path: SLSBindSurface → Wayland

`DESIGN-WINDOWSERVER-REPLACEMENT.md:56-96` documents the mac-window surfacing chain:

```
mac app (CA layer tree) → shim (WindowServer: windowID → IOSurfaceRef)
  → WS::Capture::WSCreateIOSurfaceForWindowList (per-window __IOSurface)
  → IOSurfaceGetID → PRIME_FD_TO_HANDLE (drmasahi) → zwp_linux_dmabuf_v1 create_immed
  → one wl_surface per mac window → Hyprland composites mac + Linux windows
  → shim presents Hyprland's flip via present_iosurface → IOMFB
```

On macOS 26 the legacy CGS surface registry is **empty** (`SLSGetSurfaceList` → 0 for every
window; `SKYLIGHT_SCANOUT_MACSURFACE.md`), so the on-ramp is the CA layer-tree walk
`WS::Capture::WSCreateIOSurfaceForWindowList` — `DESIGN-WINDOWSERVER-REPLACEMENT.md:35,77-80`.

### 4.5 The boot chooser

`src/chooser.c` (1516 lines) — runs **before WindowServer** and branches macOS vs shimmy:

- `claim_windowserver_service()` (`chooser.c:288`) does `bootstrap_check_in` for both lowercase
  names **before the countdown** so the real WindowServer loses the service race
  (`chooser.c:332-341`); `handoff_service_to_standin()` (`chooser.c:356`) hands the two receive
  rights to the stand-in via `MOVE_RECEIVE` (handoff ids `0x57534831`/`0x57534832`).
- `config_default_branch()` (`chooser.c:201`) reads `/usr/local/etc/shimmy/boot.conf`
  (`default=` / `once=`), clears the one-shot **before** branching.
- `present_framebuffer()` (`chooser.c:535`) blits the menu into the IOMFB framebuffer (Apple-logo
  path), with a display-less `/dev/console` + IOHIDManager keyboard fallback.
- **Keys:** `o`/`O`/`1`/`m`/`M`/Enter → macOS (escape hatch); `2`/`l`/`L` → shimmy; timeout →
  config default. Plist: `config/com.shimmy.chooser.plist`.

> Note: the boot chooser is **superseded** by the WindowServer override
> (`docs/DESIGN-BOOT-CHOOSER.md:3-7`) — see §6 dead-ends.

### 4.6 The recovery graphical installer

`docs/RECOVERY-SHELL.md` + `bin/shimmy-installer` — a minimal rootfs profile
(`./install.sh --rootfs recovery`) that boots a full-screen Qt6/PySide6 installer (via `cage`)
with WiFi connect, fresh-rootfs install, an in-app shell (gdb/objdump/python), Bluetooth/USB
device listing, and "reboot into macOS". Backend: `bin/shimmy` `cmd_recover` (`bin/shimmy:836`).

### 4.7 The syscall ABI audit + /proc, /sys, cgroup synthesis

`docs/SYSCALL-COMPLETENESS.md` (564 lines, audit date 2026-11-14). Dispatch topology:

| Route | Location |
|---|---|
| `route_syscall()` | `shim.c:187` |
| `clone(220)` `do_clone()` | `shim.c:1027` |
| sockets/epoll/poll/pipe/eventfd (26 syscalls) | `net.c:1887` `net_owns_syscall()` |
| core syscalls (66 `case` labels) | `syscalls.c:2254` `linux_syscall()` |
| long tail | `syscalls_ext.c:694` `linux_syscall_ext()` (default → `-ENOSYS` at `:1499`) |

Coverage of the 318 defined arm64 syscalls: **163 implemented (51.3%)**, 46 partial (14.5%),
71 missing/ENOSYS (22.3%), 38 refused EPERM/EOPNOTSUPP (11.9%). `/proc`·`/sys`·`/dev` are
synthesized as **leaf files** (no synthesized directory enumeration yet). Structural limitations:
signals are shadow-only (no guest signal frames), no pid namespace (children are real host
children), and the three trees lack `getdents64` synthesis (`SYSCALL-COMPLETENESS.md:42-53`).

---

## 5. WHAT'S IN PROGRESS (the current live work)

### 5.1 systemd-as-PID-1 boot — `./run.sh --init` (nearly at the login prompt)

`BOOT_TO_INIT.md` + the `--init` branch in `shim-metal-clear/run.sh:37-46` (parse) and `:53-80`
(exec `RDUID0=1 exec "$SHIM" "$ROOT" /sbin/init`). The getpid()==1 blocker is **solved**:

- `syscalls.c:344` `syscalls_guest_pid()` now returns `1` when the running guest is init
  (systemd decides "system manager" by `getpid() == 1`).

Current state (per the active session, 2026-11-15):

- ✅ **journald is ACTIVE** — the `sd_notify`/SCM_CREDENTIALS fix landed: Linux attaches
  `SCM_CREDENTIALS` to every `SO_PASSCRED` datagram; macOS has no such thing, so journald's
  `READY=1` was arriving without credentials and systemd dropped it with *"Got notification
  datagram lacking valid credential information, ignoring."* The fix recovers the sender pid from
  the autobound `AF_UNIX` source path (`net.c:895-934` `sender_ucred()`, `net.c:940`
  `autobind_sender()` — autobind on `connect()` **and** the first `sendto/sendmsg`, because
  `sd_notify()` sends without `connect`). Trace with `RDNOTIFYDBG=1` (`net.c:306-314`).
- ✅ **udev coldplug finishes** — udevd enumerates the synthetic devices.
- 🚧 **Remaining blockers:**
  1. **`tmp.mount` (mount API)** — `mount()` is a no-op success (`syscalls_ext.c:955-959`) and
     the new mount API is stubbed (`open_tree/fsopen/fsmount/fspick` at `:1667`, `move_mount`
     no-op at `:1676`, `mount_setattr` no-op at `:1764`), but `tmp.mount` still does not come up.
  2. **The CREDENTIALS step (systemd-resolved)** — resolved's notify still fails.
  3. **udevd's `READY=1` notify** — udevd's readiness datagram still does not mark the unit active.
- 🚧 **The three `Type=notify` daemons (journald / resolved / udevd) still time out** waiting for
  their readiness to be acknowledged — journald now works; resolved and udevd are the two left.

Related recent landings in the same area: `PIDFD_GET_INFO` (`syscalls.c:2617`, for systemd 261's
`pidfd_get_ppid()`/`pidfd_get_pid()`), `pidfd_open` (`syscalls_ext.c:1678`), `pidfd_send_signal`
(`:1660`), `waitid(P_PIDFD)` (`:1053-1055`).

### 5.2 WindowServer takeover (injection) — the live re-injection pending

`WINDOWSERVER_TAKEOVER_PLAN.md` (status 2026-11-15): the scanout payload is re-built and
**offset-validated headlessly** (`OVERALL_VERDICT: PASS` on the current boot, slide `0x4790000`),
and the CGS/window plane now has a first implementation embedding the ~37-function stand-in
inside the payload (`payload_cgs_standin.m`, 50 checks pass). The **live root re-injection** is
the remaining step (`WINDOWSERVER_TAKEOVER_PLAN.md:231-305`), then Route B (hook the live
`com.apple.windowserver` MIG dispatch, `:115-136`) and the HID→evdev input bridge (`:138-155`).

### 5.3 Media engine (VAAPI → VideoToolbox)

`vaapi/STATUS.md` (updated 2026-11-14): single-frame H.264 decode via ffmpeg VAAPI is
**byte-exact** (matches the `rdva_probe` reference decode). The bridge is syscall `0x2003`
(`rdva_host.h`: `RDVA_OP_QUERY/DECODE/GET_FRAME/RESET`; host `rdva_host.m` keeps a persistent
`VTDecompressionSession`). In progress: multi-frame — a **reorder-buffer hold** on the 3rd P-frame
(`VTDecompressionSessionFinishDelayedFrames` change in place, not re-tested); B-frame POC reorder;
`vaExportSurfaceHandle` zero-copy, encode, and HEVC/VP9/AV1 remain unimplemented.

---

## 6. KEY INSIGHTS / DEAD-ENDS

1. **Boot-time WindowServer replacement is BLOCKED — injection is the only path.**
   A separate-process stand-in cannot replace WindowServer at boot: it cannot carry WindowServer's
   **entitlements** (the framebuffer/HID/scene machinery are entitlement-gated to the real binary),
   the **`/Library/LaunchDaemons` override is ignored** in practice (launchd's precedence did not
   produce a working stand-in boot), **`/System` is the sealed system volume (SSV)** (read-only even
   with SIP off — the earlier in-place edit was not viable), and the **service race** is not
   reliably winnable before the display comes up. Conclusion: run the shim's code **inside the
   already-entitled, already-running WindowServer** (`WINDOWSERVER_TAKEOVER_PLAN.md:3-8`).
   The `shimmy takeover`/`release` override + chooser remain as a tracked boot-time *fallback*, not
   the primary path.

2. **The framebuffer / HID are gated to WindowServer.** Pre-WindowServer, the on-screen present
   (IOMFB + CoreDisplay) returns `kIOReturnNotPrivileged` even as root and the display id is `0x0`,
   so the chooser is display-less and reads a `/dev/console` key instead of a menu
   (`docs/DESIGN-BOOT-CHOOSER.md:13-16`, `config/com.shimmy.chooser.plist:54-71`).

3. **Better than a hypervisor** — the shim borrows the hardware directly (Metal GPU, CoreAudio,
   IOMFB display, media engine): no VM, no emulated GPU, no CPU emulation. Linux tasks are real
   Mach tasks.

4. **Better than a kext** — kexts are deprecated on Apple Silicon; the future is the **syscall
   fast path**. `fastcall.c` got syscalls **85× faster, validated, and ON BY DEFAULT** (see the
   `linux-shim`/rdmetal history: `perf(fastcall): syscalls 85x faster …`), reaching libc without
   the x30 trap-conversion.

5. **The CGS surface registry is dead on macOS 26.** `SLSGetSurfaceList` returns 0 surfaces for
   every window; the real on-ramp is the CA layer-tree walk
   (`WS::Capture::WSCreateIOSurfaceForWindowList`) — `SKYLIGHT_SCANOUT_MACSURFACE.md`.

6. **`present_iosurface` is the display's own flip** — SkyLight `+0x411F1C`, virtual
   `SLCADisplay::vtable[0x6a0]` (vtable base unslid `0x1EE402080`), flows into
   `CoreDisplay_Display_PresentIOSurface` → IOMFB swap. It is PAC-signed (arm64e) and must be
   called on the CA render thread; the earlier foreign-thread call hit the `CompositorMetal`
   `scanout_iosurface != nullptr` / `src_texture != nil && dst_texture != nil` assert (`brk #1`)
   (`…/integration/rdmetal/DESIGN.md:11-27,86-96`).

---

## 7. BUILD & RUN COMMANDS

### Build (shim source — `shim-metal-clear`)

```sh
cd /Users/dperry/Documents/Codex/2026-11-07/task/hyprland-stage/metal-wayland-next/shim-metal-clear
./build.sh cli            # shim_cli  (headless: no Metal/Cocoa)
./build.sh gui            # shim_gui  (Metal window host) + rd_mac_app/rd_mac_probe
./build.sh windowserver   # shim_windowserver + ws_test_peer + ws_real_client
./build.sh                # everything (cli, gui, wlh, capres, macapp, chooser)
```

### Build (shimmy repo)

```sh
cd /Users/dperry/rmatrixos-shim
./build.sh cli | gui | wlh | chooser | capres | handoff | macapp   # ./build.sh = all
./install.sh [--rootfs arch|omarchy|kiosk|recovery] [--dry-run]     # bootstrap the rootfs
```

### Run

```sh
# alongside macOS (WindowServer still up):
./run.sh                                   # audio up + headless (shim_cli)
./run.sh --gui -- <guest-cmd>              # Metal window host
./run.sh --rootfs omarchy                  # Hyprland + Quickshell desktop
./run.sh --rootfs arch --gui -- Hyprland   # bare base + your desktop

# REAL Linux boot (systemd as PID 1, getty -> login):
./run.sh --init            # shim_gui, visible VT login
./run.sh --init --cli      # headless variant
RDGPU_SCANOUT=1 ./run.sh --init            # takeover flip fires on whichever DE logs in
```

### Takeover / boot / release (shimmy CLI)

```sh
cd /Users/dperry/rmatrixos-shim
sudo ./bin/shimmy takeover --windowserver <path-to>/shim_windowserver   # install /Library override
sudo ./bin/shimmy release                                               # remove it
sudo ./bin/shimmy boot once shimmy        # SAFE: boot shimmy once, auto-revert to macOS
sudo ./bin/shimmy boot default macOS      # the safe default
sudo ./bin/shimmy boot default shimmy     # RISKY: persistent Linux on every boot
./bin/shimmy config validate|render       # declarative config
./bin/shimmy run                          # derive env + exec run.sh
./bin/shimmy recover [--dry-run|--scan|--ssid …|--shell|--reboot]      # recovery backend
```

---

## 8. ESCAPE / RECOVERY

See `docs/BOOT-ESCAPE.md` (and the wiki Recovery page). The takeover never edits `/System`, so
recovery is always a single `rm` of the `/Library` override:

1. **Normal** — `sudo ./bin/shimmy release`.
2. **Cmd+S** (single-user) — `rm /Library/LaunchDaemons/com.apple.WindowServer.plist; reboot`.
3. **Cmd+R** (Recovery) — mount the Data volume, then the same `rm` under
   `/Volumes/<volume>/Library/LaunchDaemons/…`.
4. **One-shot boot** — `shimmy boot once shimmy` clears itself back to `default=macOS` *before*
   branching, so the next boot (or a power-button press) is macOS with no keyboard/display.
5. **Warm-switch** — the stand-in's clean exit releases the two `com.apple.windowserver` receive
   rights; the real WindowServer can then re-check-in.

---

## 9. NEXT STEPS (handoff to the continuing agent)

1. **Finish the `--init` boot** — clear `tmp.mount` (mount API), the resolved CREDENTIALS step,
   and udevd's `READY=1` notify so the three `Type=notify` daemons stop timing out.
2. **Live WindowServer re-injection** — run the documented root injection
   (`WINDOWSERVER_TAKEOVER_PLAN.md:231-305`), then Route B (patch the server-side MIG dispatch
   table) and the HID→evdev input bridge.
3. **Wire the Metal renderer into the live guest** (currently falls back to CPU softpipe).
4. **Media engine** — re-test the P-frame reorder fix, then B-frames / zero-copy / encode.
5. **Takeover polish** — hide the macOS menu bar/Dock flashing and virtual-unplug the taken
   display (`online` flag).

**Constraints:** `/Users/dperry/linux-shim` is read-only for all current work — do not modify it.
`shim-metal-clear/src/*` regular files (not the `linux-shim` symlinks) and the `integration/rdmetal`
tree are the editable surfaces.
