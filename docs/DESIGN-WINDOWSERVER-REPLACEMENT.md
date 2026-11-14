# WindowServer replacement — shimmy *is* the WindowServer

**Direction.** Instead of injecting a payload into a still-running WindowServer (today's scanout
takeover), the shim *registers itself as* the WindowServer: it owns the Mach bootstrap service, the
CoreGraphics/SkyLight connection and surface registry, the input stream, and the display present
path. macOS's background services (launchd, Dock, SystemUIServer, loginwindow) and every AppKit app
connect to the shim's port and never see the real WindowServer. The shim forwards each mac-app
window's backing IOSurface into Hyprland as a `wl_surface`, Hyprland composites mac windows and
Linux windows into one scene, and the shim presents Hyprland's flip to the display through the
already-proven `present_iosurface` path. "Get back to macOS" is a **warm switch**, not a live
handoff: the shim exits cleanly, releases the Mach service and the display, and the real
WindowServer is relaunched. mac apps reconnect (they are not migrated), which is acceptable because
returning to macOS is a *mode change*, not a state-preserving switch.

This document is concrete and version-pinned to the reference machine: **Apple M2 Max, macOS 26.5.1
(25F80) arm64e**, SkyLight `__TEXT` base `0x186CD9000`.

---

## 1. What the shim must provide vs. what it can stub

The load-bearing insight: **the shim does not reimplement CoreAnimation compositing.** Real
WindowServer runs QuartzCore's render loop and `CompositorMetal` to composite mac windows into a
display IOSurface. The shim drops that whole half of WindowServer and delegates compositing to
Hyprland. It only has to provide enough of the CGS/SkyLight *server* surface that clients believe
WindowServer is there, plus the plumbing that turns each window's IOSurface into a Wayland buffer.

### 1.1 Must provide (real — clients fail without these)

| Subsystem | What it is | What the shim implements |
|---|---|---|
| **Mach bootstrap registration** | `com.apple.WindowServer` (system domain) and SkyLight's `__SLSERVER` / `__slserver` session service | A root LaunchDaemon (or the existing root injector's privilege) calls `bootstrap_register` with a receive right the shim owns. This is the single point of takeover: once the name resolves to the shim's port, every client connects to the shim. |
| **CGS connection registry** | `SLSMainConnectionID` / `SLSNewConnection` hand out `CGSConnectionID`s and per-connection state | Serve the CGS MIG routines on the WindowServer port: allocate connection ids, track each client's registered event port (`CGSSetEventPort` / notify port), and dispatch messages per connection. |
| **Window registry + z-order** | `CGSNewWindow`/`SLSCreateWindow`, `CGSOrderWindow`, `CGSMoveWindow`, bounds/level/tag/alpha/shadow, `SLSCopyBestManagedDisplayForRect` | Own the window tree, z-order, and window→surface map. This is the shim's source of truth for what mac windows exist and how they map to Wayland surfaces. |
| **Surface registry + IOSurface capture** | the per-window backing surface | On macOS 26 the legacy CGS surface registry is **empty** (`SLSGetSurfaceList` → 0 surfaces for every window, proven in `SKYLIGHT_SCANOUT_MACSURFACE.md`). The shim must instead walk the CA layer tree: `WS::Capture::WSCreateIOSurfaceForWindowList(WindowCaptureConfiguration&)` (called from `PresentationDisplay::Present()` @ SkyLight `+0xB0148`) is the on-ramp to each window's `CASurface`/`__IOSurface`. |
| **SkyLight display + present** | the display object and the scanout flip | Reuse the proven scanout path: resolve `SLCADisplay*` for the display, drive `present_iosurface` (SkyLight `+0x411F1C`, PAC-signed vtable slot) → `CoreDisplay_Display_PresentIOSurface` → IOMFB swap. |
| **Input / HID routing** | keyboard/pointer/touch, hit-testing, focus | Own the HID event stream (an IOHID/event tap or the loginwindow event port), hit-test mac windows against the registry, and deliver events to the frontmost client's event port — while routing non-mac input to the Linux evdev/libinput path as today. |
| **App lifecycle / activation** | app registration, `CGSSetFrontmostApplication`, activation notifications | Track running apps and frontmost state; Dock/SystemUIServer poll this via `CGWindowListCopyWindowInfo`-style calls, so the registry must answer them. |

### 1.2 Can stub or defer (safe no-ops initially)

- **Imaging/capture** — `CGWindowListCreateImage`, ScreenCaptureKit. Stub first (or return a
  readback from Hyprland's composite later). Dock thumbnails/Mission Control depend on it; they can
  be blank until phase-late.
- **Display configuration + color** — brightness, ColorSync, HDR tone-mapping, Night Shift, rotation.
  The DCP/IOMFB owns these; report the real display set and accept-and-ignore reconfig.
- **Text input / IME / dictation**, **Accessibility**, **Apple Events** — stub.
- **Spaces / Mission Control / Exposé** — stub; mac apps see a flat desktop.
- **CoreAnimation render loop / `CompositorMetal`** — *not provided at all*; Hyprland composites.

The "stub" line is deliberately aggressive: only the six rows in §1.1 block a mac app from
launching, drawing a window, receiving input, and being visible.

---

## 2. Mac-app surfacing path (CGWindow surface → IOSurface → Wayland surface)

```
mac app (AppKit/CA)
  │  CA layer tree commit → CASurface backed by an IOSurface
  ▼
shim (acting as WindowServer)           window registry: windowID → IOSurfaceRef
  │  WS::Capture::WSCreateIOSurfaceForWindowList  → per-window __IOSurface
  │  IOSurfaceGetID / IOSurfaceLookup  (send-right or id, in-process)
  ▼
shim (acting as Hyprland client)        import the IOSurface as a dma-buf
  │  PRIME_FD_TO_HANDLE (drmasahi) → carrier fd  →  zwp_linux_dmabuf_v1 create_immed
  ▼
Hyprland                                 one wl_surface per mac window
  │  composites mac windows + Linux windows → rdmetal → Metal → frame IOSurface
  ▼
shim (acting as the display driver)      present_iosurface → IOMFB swap
```

**Step 1 — surface capture.** Each mac window's live pixels are *not* in the dead CGS registry; they
are in the CA layer tree that WindowServer owns. The shim, being the WindowServer, already owns that
tree. The concrete on-ramp is `WS::Capture::WSCreateIOSurfaceForWindowList` (SkyLight, disassembled
in `SKYLIGHT_SCANOUT_FLIP3.md` and `SKYLIGHT_SCANOUT_MACSURFACE.md` §4 option 1); it walks
`CGXWindow`s and yields the per-window `CASurface`/`IOSurface`. For apps the shim itself launches, it
can instead publish the IOSurface directly (the existing `rd_mac_app` path) — no tree walk needed.

**Step 2 — IOSurface → dma-buf.** This is already proven end-to-end by the rdmetal cross-process
dma-buf work: the shim's `drmasahi.c` node issues a carrier fd for the IOSurface (`PRIME_FD_TO_HANDLE`,
with the `bo_import_shared_fd` read-only-mmap and `refs = 2` fixes from
`SKYLIGHT_SCANOUT_ROUND37_SERVER42.md`), and Hyprland 0.49 imports that fd via the standard
`zwp_linux_dmabuf_v1` version-3 `create_immed` path (`patches/hyprland-0.49-standard-dmabuf.patch`).
No CPU copy: the mac window's pixels flow to the GPU compositor as a native dma-buf. The
`IOSurfaceLock` CPU-readback branch (`mac_surface_fill_slot`) is kept only as a fallback.

**Step 3 — one `wl_surface` per window.** The shim is simultaneously the WindowServer (owns the
window registry) and a Wayland *client* of Hyprland. It keeps a `windowID ↔ wl_surface` map, creates
an `xdg-toplevel` (or layer surface) per mac window, sets geometry from the window's bounds, commits
the dma-buf buffer, and drops/reorders surfaces as `CGSOrderWindow`/`CGSMoveWindow` mutate the
registry. Hyprland composites mac windows and native Linux windows into one desktop; the shim
presents that desktop's flip to the display through the existing scanout path. The shim therefore
does **not** run a CA render loop at all — it is a control-plane server plus a surface forwarder.

---

## 3. Warm-switch exit sequence (no live handoff)

The flick-back is a controlled teardown, ordered to never leave the display dark or the service
dangling:

1. **Stop new work.** The shim stops accepting new CGS connections and new windows; it flushes any
   in-flight CA transaction and present.
2. **Tear down the Wayland side.** It destroys each `wl_surface`, releases dma-buf buffers, and
   disconnects from Hyprland. (From Hyprland's view this is a normal client exit; mac windows
   disappear from the Linux scene, Linux windows stay.)
3. **Release the display.** It stops the scanout loop, drains the final `present_iosurface`
   (`swapWait`, selector 6), and restores the display's `online`/`enabled` flags (the
   `CGXDisplayDeviceForDisplayID` node fields `info+0x128`/`+0x129` it cleared on takeover — the
   CoreDisplay path from `SKYLIGHT_SCANOUT_ROUND37_SERVER42.md`). The display is left in a state the
   real WindowServer can re-initialize via IOMFB/DCP.
4. **Release the Mach service.** It deallocates the receive right backing `com.apple.WindowServer`
   (and `__SLSERVER`/`__slserver`), so the name goes dead. This is the "release" that lets the next
   step re-register the same name.
5. **Exit.** The shim process exits cleanly (`exit(0)`).
6. **Relaunch the real WindowServer.** A LaunchDaemon job shadowing `com.apple.WindowServer` (or
   loginwindow, which normally owns the session's WindowServer) detects the dead service and
   relaunches the real `/System/Library/CoreServices/WindowServer`. It re-registers the service,
   re-initializes the display, and mac apps reconnect from scratch (AppKit treats the dead
   connection as a server restart: background services re-register; foreground apps relaunch or
   reconnect). There is **no** live handoff — the mac UI is rebuilt, not migrated — which is exactly
   the intended "you are back on macOS" experience.

Because no state is handed off, the exit sequence has no serialization/transfer step; its only
correctness requirements are *ordering* (Wayland teardown → display release → service release → exit)
and *idempotence* (the real WindowServer must boot cleanly from a released display, so the takeover
path's flag writes must be exactly reversible).

---

## 4. Phased build order

Each phase is independently testable and leaves the system revertible.

**Phase 0 — reuse the stepping stone (already proven).** Pin these as the foundation, do not rebuild:
`present_iosurface` scanout (`payload_scanout_server*.m`, `SCANOUT_SERVER_RUNBOOK.md`), display
online/enabled toggle via `CGXDisplayDeviceForDisplayID` (CoreDisplay `+0xd0b14`), the Mach
register + receive-right handoff (`register_handoff_test.c`, `ws_inject_manual_d --register`),
cross-process dma-buf (`drmasahi.c` + Hyprland standard dmabuf), and the CA-layer-tree surface
on-ramp (`WS::Capture::WSCreateIOSurfaceForWindowList`).

**Phase 1 — Mach/CGS takeover.** Register `com.apple.WindowServer` (+ `__SLSERVER`/`__slserver`)
to the shim's port; implement the connection MIG routines (`SLSMainConnectionID`/`SLSNewConnection`,
connection-id allocation, event-port registration). *Exit criterion:* a probe AppKit app connects to
the shim and receives a `CGSConnectionID`.

**Phase 2 — window + surface registry.** Implement `SLSCreateWindow`/`CGSOrderWindow`/`CGSMoveWindow`
and the z-order tree; capture each window's IOSurface via the CA layer-tree walk (or `rd_mac_app`
publishing for shim-launched apps). *Exit criterion:* the shim logs the window tree and a real
per-window IOSurface id for a stock app (e.g. Calculator).

**Phase 3 — surfacing into Hyprland.** Wire window→`wl_surface`: IOSurface → dma-buf → `create_immed`
→ `xdg-toplevel`, with geometry from the registry. *Exit criterion:* a mac app's live window appears
as a composited Wayland surface inside the Linux desktop (non-black, correct bounds).

**Phase 4 — input + app lifecycle.** Own the HID stream; hit-test against the registry; deliver to
the frontmost client's event port; serve `CGWindowListCopyWindowInfo`-style queries and frontmost
state so Dock/SystemUIServer render. *Exit criterion:* clicking/typing in a mac window works, and the
Dock enumerates mac windows.

**Phase 5 — display drive + host suppression.** Run Hyprland as the compositor of the combined scene,
present its flip via `present_iosurface`, and clear `online`/`enabled` on taken displays so no host
UI flashes through. This is the existing takeover path promoted to the replacement stack.

**Phase 6 — warm switch.** Implement §3's ordered teardown and the shadow LaunchDaemon that relaunches
the real WindowServer on shim exit. *Exit criterion:* flick-into-Linux and flick-back-to-macOS each
complete cleanly, with the real WindowServer booting from a released display and mac apps reconnecting.

**Stepping-stone map.** Phase 1 ← `register_handoff_test.c` / `e2e_mach_test.c` / `pu_probe.c`;
Phase 2 ← `SKYLIGHT_SCANOUT_MACSURFACE.md` (the registry-is-dead verdict + `WS::Capture` on-ramp);
Phase 3 ← `drmasahi.c` dma-buf fixes + `patches/hyprland-0.49-standard-dmabuf.patch` +
`SKYLIGHT_SCANOUT_SHIM_DIFF.md` (the BGRA-IOSurface `src.iosurface` contract); Phases 5 ←
`SCANOUT_SERVER_RUNBOOK.md` + `SKYLIGHT_SCANOUT_ROUND37_SERVER42.md`; Phase 6 ← the flick/suppression
model in `DESIGN.md`.
