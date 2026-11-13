# shimmy

**Run Linux natively on your Mac. No VM. No emulation.**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![shim CI](https://github.com/darrinperry83/shimmy/actions/workflows/ci.yml/badge.svg)](https://github.com/darrinperry83/shimmy/actions)
[![rdmetal build](https://github.com/darrinperry83/shimmy-rdmetal/actions/workflows/build.yml/badge.svg)](https://github.com/darrinperry83/shimmy-rdmetal/actions)
[![PRs welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

## Overview

shimmy runs an unmodified Linux userland natively on Apple Silicon. The Linux processes are not
isolated in a virtual machine; they are real tasks scheduled by the Mac's own Mach kernel.
Graphics render through Metal (the Apple GPU), audio through CoreAudio, and the display through
the Mac's own compositor and display controller. The result is a thin translation layer over the
scheduler and drivers you already have, with no CPU emulation and no virtual hardware.

Boot to a TTY, log in, and run the window manager of your choice (Hyprland, KDE, Sway, GNOME, i3,
or anything else in the Arch repositories) along with the full ARM64 package ecosystem through
`pacman`. Displays, hotplug, workspaces, audio, and input behave as if Linux were installed on the
hardware. macOS continues to run underneath, so mac applications appear as windows inside the
Linux desktop and the Mac remains fully usable.

### Status

**Pre-alpha.** The project boots a real Arch Linux rootfs and renders Hyprland, KDE Plasma, and
Omarchy with working audio, input, a TTY console, and a native DRM-master compositor that presents
on both displays. The Metal renderer and the direct-display takeover are the active frontier.

**Tested on:** Apple M2 Max, 96 GB MacBook Pro, macOS 26.5.1 (arm64e).

## Contents

- [Why shimmy](#why-shimmy)
- [GPU and performance](#gpu-and-performance)
- [Features](#features)
- [How it works](#how-it-works)
- [Architecture](#architecture)
- [Quick start](#quick-start)
- [Presentation modes](#presentation-modes)
- [Development](#development)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [Security](#security)
- [License](#license)

## Why shimmy

macOS is a great host, but many workloads need real Linux: the toolchain, a tiling window manager,
server workloads, and the freedom of an open stack. The existing options trade something away.
Virtual machines are slow and lack a direct GPU. Dual-booting loses the Mac. shimmy runs the Linux
userland directly on the Mac, scheduled by the Mach kernel and driven by the real Metal, CoreAudio,
and IOMFB drivers, so Linux feels like it is on the hardware while macOS stays fully usable.

## GPU and performance

shimmy does not emulate a GPU. The `rdmetal` driver is a Mesa Gallium driver that translates the
guest's OpenGL and EGL into Metal, so the compositor's rendering runs on the Apple Silicon GPU.

- **Render path:** guest shaders -> NIR -> SPIR-V -> Metal Shading Language (spirv-cross) -> the Apple GPU.
- **Current state:** clear, draw, texture sampling, vertex attributes, index buffers, and uniforms
  are Metal-backed and verified. The compositor renders through the real GPU, not a software
  rasterizer.
- **Target:** full Metal-backed rendering at the display's refresh rate.

This is the key difference from a virtual machine. There is no emulated GPU and no guest
GPU-driver guessing; the real Apple GPU does the work through Metal.

## Features

| Area | Status | Notes |
|---|---|---|
| Arch Linux (aarch64, glibc) | ✅ | bootstrapped locally, stock `pacman` packages |
| `rdmetal` graphics driver | ✅ | Mesa Gallium + DRM/GBM node, installed as the default GL driver (no env vars) |
| Native DRM-master compositor | ✅ | Hyprland/KDE run as the DRM master via `MODE_ATOMIC` |
| Multi-display + per-CRTC present | ✅ | external 3440x1440 + builtin 3600x2338, both present |
| Metal-backed rendering | ✅ | clear + draw + texture + vertex/index/uniform through Metal |
| Input (evdev/libinput) | ✅ | keyboard + pointer in native DRM-master mode |
| Audio | ✅ | PipeWire/ALSA -> CoreAudio, shim-generated card, auto sink |
| TTY / virtual console | ✅ | `/dev/ttyN` + getty login + Ctrl+Alt+Fn switching |
| mac apps as native windows | ✅ | owned by the compositor as Wayland surfaces |
| Cross-process dma-buf | ✅ | accelerated client buffers |
| Direct-display takeover | 🚧 | `present_iosurface` flip fires (`ret=1`); repeatable-frame stability in progress |
| Omarchy rootfs | 🚧 | optional `--rootfs omarchy` (Hyprland + Quickshell) |

## How it works

Read [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md) for the technical story: how the shim translates
the Linux kernel surface onto the Mach scheduler and the real macOS drivers.

## Architecture

```
        Linux guest (Arch, aarch64)
        TTY -> Hyprland / KDE -> apps
                    |
     syscalls · Wayland · DRM · /dev/snd
                    |
        +-----------v-----------+
        |   shim (macOS)        |
        |  · syscall translation|
        |  · native DRM master  |
        |  · input / VT / audio |
        |  · Metal present      |
        +-----------+-----------+
                    | IOSurface
        +-----------v-----------+
        |   rdmetal (driver)    |   (we wrote this)
        |  Mesa Gallium + DRM   |
        +-----------+-----------+
                    |
        +-----------v-----------+
        |   scanout (injection) |   (we wrote this too)
        |  present_iosurface    |
        +-----------------------+
```

See [DESIGN.md](DESIGN.md) for the full architecture and the native-machine target.

## Quick start

```sh
git clone --recursive https://github.com/darrinperry83/shimmy.git
cd shimmy
./install.sh                  # builds the shim and bootstraps the Arch rootfs
./install.sh --rootfs omarchy # (optional) the Omarchy rootfs (Hyprland + Quickshell)
./run.sh                      # boots to the TTY login
```

Inside the guest, install the desktop of your choice through the package manager:

```sh
pacman -S hyprland           # or plasma, sway, i3, ...
```

There are exactly two rootfs choices, `arch` (the bare base) and `omarchy` (Arch + Hyprland +
Quickshell). The desktop is always installed by you through `pacman`; there is no `--desktop`
flag. The rootfs is built locally and is not stored in git.

See [Host setup](README.md#host-setup--sip--the-scanout-injection-one-time) for the one-time SIP
and scanout prerequisites.

## Presentation modes

| Mode | Behavior |
|---|---|
| **Windowed** (default) | the desktop runs in a normal window, WindowServer present |
| **Takeover** | suppress WindowServer, `present_iosurface` flips directly to all displays |

## Development

The project uses a trunk-based-on-stable branching model:

- `main`: stable, tagged releases.
- `develop`: staging; where pull requests land and CI runs before a release.
- `experimental`: risky or forward work, such as the scanout takeover and the Metal renderer.

**Workflow:** branch from `develop`, open a pull request (CI runs), review, then merge to
`develop`. When a release is cut, `develop` is merged to `main` and tagged. See
[CONTRIBUTING.md](CONTRIBUTING.md) for the full process.

```sh
./build.sh gui     # shim_gui: the display-server host
./build.sh cli     # shim_cli : headless guest runner
```

## Roadmap

| Item | Priority |
|---|---|
| Repeatable scanout flip (frame 1..N stability) | high |
| Guest `glReadPixels` readback | medium |
| Hotplug uevent | medium |
| Full plasmashell EGL session | medium |
| Full Omarchy application set | medium |
| Workspaces across displays | medium |
| System tray + mac integration | medium |
| Chromium `/proc` gap | medium |

## Contributing

Contributions are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) for the workflow and
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for the community standards.

## Security

Report vulnerabilities privately. See [SECURITY.md](SECURITY.md).

## License

[GPLv3](LICENSE).

**Attribution.** In addition to the terms of GPLv3, and under its Section 7(b), you must preserve
the following attribution in the Appropriate Legal Notices displayed by any work that contains
this software:

> shimmy - run Linux natively on your Mac.
> Copyright (C) 2025 Darrin Perry.
> https://github.com/darrinperry83/shimmy

In practice this means any interactive interface (an about box, a legal-notices screen, or a
prominent `show w`/`show c` equivalent) must credit shimmy and its author.
