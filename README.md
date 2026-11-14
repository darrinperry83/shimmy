# shimmy

**Native Linux on your Mac. No VM. No emulation.**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![shim CI](https://github.com/darrinperry83/shimmy/actions/workflows/ci.yml/badge.svg)](https://github.com/darrinperry83/shimmy/actions)
[![rdmetal build](https://github.com/darrinperry83/shimmy-rdmetal/actions/workflows/build.yml/badge.svg)](https://github.com/darrinperry83/shimmy-rdmetal/actions)
[![PRs welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

## Overview

shimmy runs an unmodified Linux userland natively on Apple Silicon. The Linux processes are not
isolated in a virtual machine; they are real tasks scheduled by the Mac's own Mach kernel.
Graphics render through Metal on the Apple GPU, audio through CoreAudio, and the display through
the Mac's own IOMFB display controller. The result is a thin translation layer over the scheduler
and drivers you already have, with no CPU emulation and no virtual hardware.

Boot to a TTY, log in, and run the window manager of your choice (Hyprland, KDE, Sway, GNOME, i3,
or anything else in the Arch repositories) along with the full ARM64 package ecosystem through
`pacman`. Displays, hotplug, workspaces, audio, and input behave as if Linux were installed on the
hardware. macOS continues to run underneath, so mac applications appear as windows inside the
Linux desktop and the Mac remains fully usable.

### Status

**Pre-alpha, but the core works.** It boots a real Arch rootfs, renders Hyprland, KDE, and Omarchy
on both displays through a native DRM-master compositor, and has working audio, input, a TTY
console, and mac-app integration. The scanout takeover (the direct `present_iosurface` flip) is
stable and visible. The Metal renderer is complete, but the live guest still falls back to CPU
softpipe. The active frontier is polishing the takeover (hiding the macOS UI and removing the
taken-over display from macOS) and wiring the Metal renderer into the live guest.

See [Feature support](docs/FEATURES.md) for the current status of every subsystem.

**Tested on:** Apple M2 Max, 96 GB MacBook Pro, macOS 26.5.1 (arm64e).

## Contents

- [Why shimmy](#why-shimmy)
- [GPU and performance](#gpu-and-performance)
- [Features](#features)
- [How it works](#how-it-works)
- [Architecture](#architecture)
- [Quick start](#quick-start)
- [Modes](#modes)
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
guest's OpenGL and EGL into Metal, so the compositor's rendering targets the Apple Silicon GPU.

- **Render path:** guest shaders -> NIR -> SPIR-V -> Metal Shading Language (spirv-cross) -> the Apple GPU.
- **Current state:** clear, draw, texture sampling, vertex attributes, index buffers, and uniforms
  are Metal-backed and verified. The renderer is complete, but the live guest still runs on CPU
  softpipe until the Metal path is wired in.
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
| Metal-backed rendering | ✅ | clear + draw + texture + vertex/index/uniform through Metal (headless-validated) |
| Live guest GPU acceleration | 🚧 | the live compositor still falls back to CPU softpipe until the Metal path is wired in |
| Input (evdev/libinput) | ✅ | keyboard + pointer in native DRM-master mode |
| Audio | ✅ | PipeWire/ALSA -> CoreAudio, shim-generated card, auto sink |
| TTY / virtual console | ✅ | `/dev/ttyN` + getty login + Ctrl+Alt+Fn switching |
| mac apps as native windows | ✅ | owned by the compositor as Wayland surfaces |
| Cross-process dma-buf | ✅ | accelerated client buffers |
| Direct-display takeover | ✅ | stable + visible direct `present_iosurface` flip (held up overnight) |
| Takeover: hide macOS UI | 🚧 | the Apple menu bar / Dock still flash through between flips |
| Takeover: remove display from macOS | 📋 | clear the display's `online` flag so macOS stops seeing it |
| Omarchy rootfs | ✅ | optional `--rootfs omarchy` (Hyprland + Quickshell) |

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
./install.sh                   # builds the shim + bootstraps the Arch rootfs
./install.sh --rootfs omarchy  # (optional) Arch + Hyprland + Quickshell
./install.sh --rootfs recovery # (optional) the graphical recovery shell
./run.sh                       # runs Linux alongside macOS (WindowServer still up)
```

The desktop is installed by you through the package manager; there is no `--desktop` flag:

```sh
pacman -S hyprland            # or plasma, sway, i3, ...
```

Rootfs profiles: `arch` (bare base), `omarchy` (Hyprland + Quickshell), `recovery` (a graphical
installer with WiFi + rootfs reinstall + a shell). The rootfs is built locally, not stored in git.

### Boot it for real (the takeover)

`run.sh` runs Linux *alongside* a running macOS. To boot it for real — WindowServer never starts,
the shim is the WindowServer stand-in, and mac apps run inside Linux — install the boot chooser
and reboot:

```sh
./build.sh windowserver  # in the shim tree, builds the WindowServer stand-in
sudo ./bin/shimmy takeover --windowserver <path-to>/shim_windowserver
# reboot, then pick "shimmy" at the chooser (macOS is the 10s default)
```

The chooser is reversible (`sudo ./bin/shimmy release`) and fail-safe: if the stand-in crashes, the
watchdog relaunches the real WindowServer, so macOS always comes back. See
[DESIGN-WINDOWSERVER-REPLACEMENT.md](docs/DESIGN-WINDOWSERVER-REPLACEMENT.md) and
[DESIGN-BOOT-CHOOSER.md](docs/DESIGN-BOOT-CHOOSER.md).

### Declarative config

The whole system is one file, `shimmy.yaml` (the Nix-like model — see [docs/DESIGN-NIX.md](docs/DESIGN-NIX.md)):

```yaml
rootfs: omarchy
windowManager: hyprland
mode: single-display
takeover: { displays: [external] }
flick: { key: ctrl-alt-f }
```

```sh
./bin/shimmy config validate   # check it
./bin/shimmy run               # boot with the config's mode + displays + flick
./bin/shimmy build             # build the configured rootfs profile
```

Track the work on the [GitHub project](https://github.com/users/darrinperry83/projects/2).

## Modes

Linux is the primary experience; macOS is the hardware layer underneath. A single **flick**
switches between them (a key combo, or moving the pointer across the screen boundary).

| Mode | macOS side | Linux side |
|---|---|---|
| **Windowed** | full macOS | Linux in a window (one display), fullscreen-able |
| **Single-display takeover** | macOS on the builtin | Linux owns the display of your choice |
| **Kiosk** | macOS on the builtin | one locked-down app on one display |
| **All-displays takeover** | suppressed | Linux owns all displays |

The takeover modes make the shim the WindowServer stand-in (macOS background services and mac apps
connect to the shim), and each mac app's window surfaces as a native Wayland window in the Linux
compositor. The flip is stable and visible today; a warm-switch (the flick) returns to macOS. See
[DESIGN.md](DESIGN.md) for the full mode and flick design.

## Development

The project uses a trunk-based-on-stable branching model:

- `main`: stable, tagged releases.
- `development`: where the current work lands (the active branch).
- `experimental`: risky or forward work.

**Workflow:** branch from `development`, open a pull request (CI runs), review, then merge to
`development`. When a release is cut, `development` is merged to `main` and tagged. See
[CONTRIBUTING.md](CONTRIBUTING.md) for the full process.

```sh
./build.sh gui         # shim_gui: the display-server host
./build.sh cli         # shim_cli : headless guest runner
./build.sh windowserver # shim_windowserver: the WindowServer stand-in
```

## Roadmap

The roadmap is tracked on the [GitHub project](https://github.com/users/darrinperry83/projects/2).
The current frontier:

| Item | Priority |
|---|---|
| Live boot-time takeover proof (a real Dock/AppKit connects + renders) | high |
| Media engine: zero-copy + encode + HEVC/VP9/AV1 + ffmpeg/mpv plumbing | high |
| Complete the Linux syscall ABI (`io_uring`, `ptrace`/`seccomp`, signal delivery, `/proc/net`) | high |
| Versioned update + rollback (`shimmy.lock`, atomic fallback) | high |
| Bluetooth + USB device bridges | medium |
| Proper install area on APFS | medium |
| Syscall fast path + guest heap fix (performance) | medium |

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
