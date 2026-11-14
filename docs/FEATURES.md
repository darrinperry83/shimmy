# Feature support

This page tracks what shimmy does right now, and the status of each subsystem. The model is
borrowed from the [Asahi Linux feature-support pages](https://asahilinux.org/docs/platform/feature-support/overview/):
a status legend, a per-subsystem table, and notes for the caveats.

## What works right now

| Capability | Status |
|---|---|
| Boots Arch Linux (aarch64, glibc) to a TTY login | ✅ |
| Native DRM-master compositor (Hyprland / KDE), both displays | ✅ |
| Metal renderer (clear, draw, texture, vertex, index, uniform), headless-validated | ✅ |
| Audio (PipeWire -> CoreAudio) | ✅ |
| Input (evdev/libinput keyboard + pointer) | ✅ |
| TTY / virtual console with Ctrl+Alt+Fn | ✅ |
| mac apps as Wayland surfaces | ✅ |
| Cross-process dma-buf (accelerated clients) | ✅ |
| Scanout takeover: stable + visible direct flip | ✅ |
| Omarchy rootfs (`--rootfs omarchy`) | ✅ |
| Self-contained build + GitHub CI | ✅ |
| Live guest running GPU-accelerated (not softpipe) | 🚧 |
| Takeover: hide macOS UI (no menu bar / Dock flashing) | 🚧 |
| Takeover: remove the external display from macOS (virtual unplug) | 🚧 |

## Status legend

| Mark | Meaning |
|---|---|
| ✅ | **Stable** | works in the current build and is verified |
| 🚧 | **In progress** | actively developed, not yet stable |
| 📋 | **Planned** | on the roadmap, not yet started |
| ➖ | **N/A** | the Mac handles this directly; the shim does not bridge it |

There are no release estimates for in-progress items. A subsystem is marked stable only when it
has been verified end to end on the test machine.

## Subsystem support

| Subsystem | Status | Bridge |
|---|---|---|
| CPU scheduling | ✅ | Linux processes run as Mach tasks; the Mach kernel schedules them |
| GPU (rdmetal) | ✅ | Mesa Gallium driver that lowers guest GLES into Metal (clear, draw, texture, vertex, index, uniform), headless-validated |
| GPU in the live guest | 🚧 | the driver is complete, but the live compositor still falls back to CPU softpipe until the Metal path is wired into the live session |
| Display (compositor) | ✅ | the compositor runs as the native DRM master, presents through the shim's Metal path |
| Multi-display | ✅ | per-CRTC present on the external (3440x1440) and builtin (3600x2338) |
| Display takeover (flip) | ✅ | direct `present_iosurface` -> IOMFB; stable and visible, held up overnight |
| Takeover host suppression | 🚧 | the Apple menu bar / Dock still flash through between flips |
| Takeover display removal | 🚧 | virtual unplug of the external from macOS (clear the `online` flag) so macOS stops seeing it |
| Audio | ✅ | PipeWire/ALSA -> CoreAudio, shim-generated sound card |
| Input | ✅ | evdev/libinput fed from the mac's HID events |
| TTY / virtual console | ✅ | `/dev/ttyN` + getty login + Ctrl+Alt+Fn switching |
| Networking | ✅ | socket translation (`net.c`), enough for `pacman` and package installs |
| Storage | ✅ | the Arch rootfs on the host filesystem |
| mac apps | ✅ | mac applications owned by the compositor as Wayland surfaces |
| Cross-process dma-buf | ✅ | accelerated client buffers across processes |

## Tested hardware

| Device | Status |
|---|---|
| Apple M2 Max, 96 GB MacBook Pro, macOS 26.5.1 (arm64e) | ✅ the reference test machine |

## Notes

- **Metal renderer.** Clear, draw, texture sampling, vertex attributes, index buffers, and a
  bounded matrix uniform all run through Metal, validated headless with a non-black textured
  indexed quad. The live guest still uses softpipe, so it renders slowly (fractional fps) until
  the Metal path is wired into the live session.
- **Guest `glReadPixels`.** Still returns black; the host-side IOSurface read is authoritative.
  This readback path is a separate, known gap.
- **Display takeover.** The direct `present_iosurface` flip is stable and visible (it held up
  overnight). The remaining takeover work is the host-UI suppression (menu bar / Dock flashing),
  the display removal (a virtual unplug of the external), and the nested Wayland-1 window
  suppression.
- **Takeover split mode.** The first takeover mode keeps the builtin display as macOS and takes
  the external for Linux; the external is then dropped from macOS via its `online` flag while the
  direct present keeps scanning out to it.
- **Hotplug.** Physical-display hotplug events are not yet synthesized; they are on the roadmap.
- **Input in takeover mode.** The evdev/libinput path is wired; the nested-Wayland `wl_seat` is no
  longer served in native-DRM or takeover mode.
