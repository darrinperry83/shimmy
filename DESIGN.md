# Design: shimmy

The goal: run Linux on macOS **indistinguishably from a native machine**. Linux is the primary
experience; macOS is the hardware layer underneath (the Mach scheduler + the real drivers).
Boot → TTY login → desktop → workspaces, with four modes (windowed / single-display takeover /
kiosk / all-displays takeover) and a **flick** that switches between Linux and macOS, all
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
│  • Wayland server (wlsrv): windowed mode                    │
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

## Product modes and the flick

The product has four modes. Linux is the primary experience; macOS is the hardware layer
underneath. A single **flick** gesture switches between Linux and macOS, and it is the product's
most important interaction.

| # | Mode | macOS side | Linux side |
|---|---|---|---|
| 1 | `windowed` | full macOS | Linux in a window (one display) |
| 2 | `takeover` (single-display) | macOS on the builtin | Linux owns one display (full desktop) |
| 3 | `kiosk` | macOS on the builtin | one locked-down app on one display |
| 4 | `takeover` (all-displays) | suppressed | Linux owns all displays |

### The flick

- Multiple screens: the flick is **spatial** (move the pointer or focus across the screen
  boundary), plus a key that swaps which display is Linux.
- Single screen: the flick is a **mode toggle** (a key that flips the whole screen between Linux
  and macOS).
- One consistent gesture everywhere: a configurable key combo, with the pointer owned by
  whichever side is active.
- The flip is near-instant because macOS is never killed. Suppression is a reversible flag (the
  display's `online` state + WindowServer's present path), and both OSes keep running underneath.
  Flicking feels like switching full-screen spaces, not rebooting.

### `windowed`

Linux runs inside a resizable macOS window (one Linux display), fullscreen-able, with macOS
running normally. Stretch it across N mac displays and the shim maps each covered display to a
Linux monitor. The pointer flicks back to macOS when it leaves the window or via the key.

### `takeover` (single-display)

The display of the user's choice is taken over by Linux and **removed from macOS entirely** (a
virtual unplug), while the other display stays macOS. The Linux compositor owns the taken-over
display and flips its frames directly via `present_iosurface` → IOMFB, bypassing WindowServer.
Result: macOS on one screen, a full Linux desktop on the other — two computers on two screens.

### `kiosk`

A single-display takeover whose Linux side runs one locked-down, always-on application (a
dashboard, signage, or point-of-sale app) instead of a desktop: no shell, no switching, nothing
to escape to. macOS runs the operator's display.

### `takeover` (all-displays)

Every display is taken over by Linux and macOS is suppressed. This is the "it's a Linux machine"
mode: boot to a TTY, log in, and you are on a Linux desktop with macOS just underneath, one flick
away.

### Shape

**One native compositor, two present sinks**: a window (windowed) or the raw displays (takeover /
kiosk). A mode flag switches the sink and which displays are taken over; the DRM-master and
connector discovery are shared. Kiosk is the single-display takeover with a different rootfs and
an "open in kiosk" flag.

## Target experience (native-equivalent)

1. **Boot** → the guest boots to the TTY console.
2. **TTY / VT** → real `/dev/tty1…ttyN` virtual consoles with `getty` login and
   `Ctrl+Alt+F1…F6` switching.
3. **Login → desktop** → log in, then `Hyprland` / `startplasma-wayland` (or auto-start).
4. **Workspaces** → the compositor's virtual desktops + shortcuts work across the display(s).
5. **Audio / input / GPU** → PipeWire→CoreAudio, native cursor/keyboard, rdmetal rendering.

## Current state vs target

Working: Arch boot, rdmetal driver (default GL, no env vars), stock Hyprland + KDE desktops,
windowed presentation, mac apps owned by the compositor, ALSA/PipeWire→CoreAudio, D-Bus session,
the TTY/VT console, the native DRM-master compositor on both displays, evdev/libinput input, the
Metal renderer (headless-validated), cross-process dma-buf, and a stable + visible scanout takeover
(the direct `present_iosurface` flip held up overnight).

Remaining:

1. **Live GPU acceleration**: wire the Metal renderer into the live guest (it still uses softpipe).
2. **Takeover host suppression**: hide the macOS menu bar / Dock so they stop flashing through.
3. **Takeover display removal**: clear the external's `online` flag so macOS stops seeing it.
4. **Guest `glReadPixels` readback** and **hotplug uevent**.
5. **Full plasmashell EGL** and the full Omarchy application set.
6. **App layer polish**: Chromium `/proc` gap, system tray with mac integration.
