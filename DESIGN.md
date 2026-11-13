# Design — shimmy

The goal: run Linux on macOS **indistinguishably from a native machine**. Boot → TTY login →
desktop → workspaces, with two presentation modes (windowed / full-display takeover), all
rendered by the **rdmetal** driver and presented by the shim.

## Layers

```
┌─────────────────────────────────────────────────────────────┐
│  Linux guest (Arch, aarch64, glibc)                          │
│  TTY console (getty)  →  Hyprland / KDE Plasma  →  apps     │
└───────────────────────────────┬─────────────────────────────┘
                                │ syscalls / Wayland / DRM
┌───────────────────────────────▼─────────────────────────────┐
│  shim (macOS)                                                │
│  • syscall translation + /dev/ttyN + /dev/dri + /dev/snd    │
│  • Wayland server (wlsrv) — windowed mode                    │
│  • input (cursor, keyboard, VT switching)                    │
│  • Metal present path (CAMetalLayer)                         │
└───────────────────────────────┬─────────────────────────────┘
                                │ IOSurface handoff
┌───────────────────────────────▼─────────────────────────────┐
│  rdmetal (Mesa Gallium driver + DRM/GBM node)               │
│  • renders the compositor's GL/EGL through the Metal host-call│
│  • presents /dev/dri/renderD128 as the guest GPU             │
└───────────────────────────────┬─────────────────────────────┘
                                │
┌───────────────────────────────▼─────────────────────────────┐
│  scanout (WindowServer injection)                            │
│  • takeover mode: suppress WindowServer, present_iosurface   │
│    flips each frame directly to the display(s)               │
└─────────────────────────────────────────────────────────────┘
```

## Architectural north star: native compositor, no custom Wayland server

The long-term target is for the compositor to run **natively**, not as a nested client of a
custom server:

```
Hyprland / KDE  (native DRM master + its own Wayland server)
   → /dev/dri/card0 (rdmetal)  → GBM  → render
   → KMS/atomic page-flip      → rdmetal → Metal → display
```

This retires `wlsrv.c`'s *presentation* role (the nested-Wayland dmabuf import). The shim keeps
its other roles: input routing, cursor compositing, and the mac-app-window→Wayland-surface
bridge (the shim acting as a Wayland *client* of the native compositor).

To get there:
1. rdmetal's DRM node becomes a full KMS/atomic device (modeset, page-flip).
2. A `libseat` shim so the compositor's DRM backend can take the seat.
3. Remove `wlsrv`'s presentation path; keep input/cursor/mac-window.

## The two presentation modes

The user chooses the mode **and** the displays:

- **`windowed`** — the desktop runs in a normal window; WindowServer present.
- **`takeover`** — suppress WindowServer; `present_iosurface` flips directly to the displays.

The user also selects **how many and which** displays the desktop uses (e.g. one display,
the external only, or all displays). This is a runtime flag, not a build-time choice.

The nested Wayland backend (`wlsrv` presentation) is retired — the compositor is the native
DRM master; `wlsrv` keeps only input, cursor, and the mac-window→Wayland-surface bridge.

## Target experience (native-equivalent)

1. **Boot** → the guest boots to the TTY console.
2. **TTY / VT** → real `/dev/tty1…ttyN` virtual consoles with `getty` login and
   `Ctrl+Alt+F1…F6` switching.
3. **Login → desktop** → log in, then `Hyprland` / `startplasma-wayland` (or auto-start).
4. **Workspaces** → the compositor's virtual desktops + shortcuts work across the display(s).
5. **Audio / input / GPU** → PipeWire→CoreAudio, native cursor/keyboard, rdmetal rendering.

## Current state vs target

Working: Arch boot, rdmetal driver (default GL, no env vars), stock Hyprland + KDE desktops,
windowed presentation, mac apps owned by the compositor, ALSA/PipeWire→CoreAudio, D-Bus session.

Remaining (the native-feel gap):

1. **TTY/VT console** — `/dev/ttyN` + getty + VT switching + a text-console renderer.
2. **Takeover mode** — dual-output (per-display size), dual handoff, dual `present_iosurface` flip.
3. **Windowed/takeover switch** — a clean mode flag.
4. **Metal-backed GLES** — replace CPU softpipe with the Metal host-call renderer (perf).
5. **Cross-process dma-buf** — client apps render accelerated (no software QtQuick).
6. **App layer polish** — Chromium `/proc` gap, system tray with mac integration.
