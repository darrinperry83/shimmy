# Feature support

This page tracks which hardware and software subsystems shimmy bridges, and the status of each.
The model is borrowed from the [Asahi Linux feature-support pages](https://asahilinux.org/docs/platform/feature-support/overview/):
a status legend, a per-subsystem table, and notes for the caveats.

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

The shim translates the Linux kernel surface onto the Mac's own scheduler and drivers. The table
below lists each subsystem, its status, and the bridge that implements it.

| Subsystem | Status | Bridge |
|---|---|---|
| CPU scheduling | ✅ | Linux processes run as Mach tasks; the Mach kernel schedules them |
| GPU (OpenGL/EGL) | ✅ | `rdmetal`, a Mesa Gallium driver that lowers guest shaders into Metal (clear, draw, texture, vertex, index, uniform) |
| Display (compositor) | ✅ | the compositor runs as the native DRM master and presents through the shim's Metal path |
| Multi-display | ✅ | per-CRTC present on the external (3440x1440) and builtin (3600x2338) displays |
| Display takeover | 🚧 | `present_iosurface` -> IOMFB, with WindowServer suppressed; the repeatable flip is in progress |
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
  bounded matrix uniform all run through Metal. Guest `glReadPixels` still returns black; the
  host-side IOSurface read is authoritative. That readback path is a separate, known gap.
- **Display takeover.** The direct `present_iosurface` flip is the active frontier. The frame
  lands (`ret=1`), and the surface lifecycle is being stabilized so the flip repeats indefinitely.
- **Hotplug.** Physical-display hotplug events are not yet synthesized; they are on the roadmap.
- **Input in takeover mode.** The evdev/libinput path is wired; in the nested-Wayland fallback the
  outer `wl_seat` is no longer served, so native-DRM mode uses the evdev path directly.
