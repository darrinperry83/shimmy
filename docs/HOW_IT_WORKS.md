# How shimmy works: the technical story

shimmy isn't a VM and doesn't emulate a GPU. It runs an unmodified Linux userland directly on
the mac and drives the **real macOS graphics and audio stack**: Metal, IOSurface, the display
controller (IOMFB), and CoreAudio: through a set of bridges it owns.

## The big picture

```
Linux guest (aarch64, glibc)
   ├─ OpenGL/EGL  →  rdmetal driver  →  Metal (the Apple GPU)
   ├─ Wayland     →  wlsrv server     →  IOSurface  →  presentation
   └─ ALSA        →  /dev/snd         →  CoreAudio
```

## 1. The GPU is real Metal

The guest believes it has a GPU because the shim presents a **DRM render node**
(`/dev/dri/renderD128`) via `drmasahi.c`. The **rdmetal** Mesa Gallium driver is the default GL
driver in the guest, and instead of a hardware ioctl backend it speaks to a **Metal host-call
broker** (guest syscall `0x2002`):

- guest shaders are lowered `NIR → SPIR-V → Metal Shading Language` (spirv-cross),
- draw/clear/resource commands cross the broker into host Metal,
- the result lands in an **IOSurface**.

So the compositor's GPU work runs on the **Apple Silicon GPU through Metal**: not a software
rasterizer. (Status: the clear+draw path is Metal-backed and verified; the texture/index-buffer
surface is the active piece of work. Until it lands, the guest falls back to softpipe.)

## 2. Presentation uses the real compositor + IOSurface

The shim's `wlsrv` is a Wayland server the compositor (Hyprland/KDE) connects to as a client.
Each frame is an IOSurface: the same `IOSurface` type macOS uses everywhere: handed through
the shim's Metal present path (`gpuwin_present` → a `CAMetalLayer`). In **windowed mode**
WindowServer composites that CAMetalLayer like any other app window.

## 3. The scanout drives the real display controller (IOMFB)

In **takeover mode**, the shim bypasses WindowServer entirely. It injects a small payload into
WindowServer (the `scanout` submodule) and calls `SLCADisplay::present_iosurface`: SkyLight's
internal present: directly. That flows into `CoreDisplay_Display_PresentIOSurface` and the
**IOMobileFramebuffer (IOMFB)** swap, which is the actual hardware path that flips the frame to
the display's scanout. This is the same path macOS's own compositor uses: the shim just drives
it itself, suppressing WindowServer.

## 4. Audio is CoreAudio

The shim presents a synthetic ALSA card (`/dev/snd`, `snd_alsa.c`); the guest's PipeWire/
WirePlumber stack renders to it as a normal Linux sound card. The PCM ring buffer is pulled by a
host **AudioUnit** (CoreAudio), so audio plays through the mac's real sound hardware.

## 5. Input is the mac's HID

The cursor and keyboard come from macOS NSEvents; the shim translates them into the guest's
Wayland pointer/keyboard and, on the text consoles, into the VT keyboard. The TTY/VT consoles
(`/dev/ttyN`) are real pty pairs, so the guest's termios/line discipline behaves exactly like
Linux.

## Why this is "native"

Every subsystem maps onto a real macOS driver or service:

| Guest side | macOS side |
|---|---|
| OpenGL/EGL | Metal (the Apple GPU) |
| frame buffer | IOSurface |
| display flip | IOMFB / the display controller |
| audio | CoreAudio / AudioUnit |
| input | NSEvent / HID |

Nothing is a userspace emulator. The bridges (`rdmetal`, the shim, the scanout) translate
protocols and reuse macOS's own kernel drivers.
