# shimmy

**Run Linux natively on your Mac — no VM, no emulation.**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![shim CI](https://github.com/darrinperry83/shimmy/actions/workflows/ci.yml/badge.svg)](https://github.com/darrinperry83/shimmy/actions)
[![rdmetal build](https://github.com/darrinperry83/shimmy-rdmetal/actions/workflows/build.yml/badge.svg)](https://github.com/darrinperry83/shimmy-rdmetal/actions)
[![PRs welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

shimmy runs Linux **natively** on Apple Silicon. The Linux processes are not inside a VM —
they are real tasks scheduled by the Mac's own **Mach kernel**. Graphics render through
**Metal** (the Apple GPU), audio through **CoreAudio**, the display through the Mac's own
compositor and display controller. It is a thin translation layer over the scheduler and
drivers you already have — **no CPU emulation, no virtual hardware**.

> **What it's going to bring:** a real Linux desktop on the Mac. Boot to a TTY, log in, and
> you're in Hyprland, KDE, or Omarchy — with displays, hotplug, workspaces, audio, and input
> all working like it's installed on the hardware. And macOS keeps running underneath, so you
> lose nothing: mac apps appear as windows inside that Linux desktop, and the Mac is still the
> Mac whenever you want it.
>
> **Status: pre-alpha.** It boots real Arch and renders Hyprland/KDE/Omarchy with working
> audio, input, and a TTY console; the native DRM-master compositor path is landed. Metal
> rendering and the direct-display takeover are the active frontier.
>
> **Tested on:** Apple M2 Max · 96 GB MacBook Pro · macOS 26.5.1 (arm64e).

---

## Contents

- [Why shimmy](#why-shimmy)
- [Features](#features)
- [How it works (technical)](docs/HOW_IT_WORKS.md)
- [Architecture](#architecture)
- [Quick start](#quick-start)
- [Presentation modes](#presentation-modes)
- [Development](#development)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [Security](#security)
- [License](#license)

## Why shimmy

macOS is a great host, but some workloads want a real Linux desktop. Existing options are VMs
(slow, no direct GPU) or dual-boot (loses the mac). shimmy runs an **unmodified** Linux
userland directly on the mac, with a native graphics driver, native audio, and native input —
so it feels like Linux was installed on the hardware, while the mac stays fully usable.

## Features

| Area | Status | Notes |
|---|---|---|
| Arch Linux (aarch64, glibc) | ✅ | bootstrapped locally, stock `pacman` packages |
| **rdmetal** graphics driver | ✅ | Mesa Gallium + DRM/GBM node, installed as the default GL driver (no env vars) |
| Hyprland desktop | ✅ | stock `hyprland` package, renders + input + cursor |
| KDE Plasma desktop | ✅ | stock `plasma-meta`, kwin_wayland + plasmashell |
| Audio | ✅ | PipeWire/ALSA → CoreAudio, shim-generated card, auto sink + RUNNING graph |
| mac apps as native windows | ✅ | owned by the compositor as Wayland surfaces (launch + IOSurface content) |
| TTY / virtual console | ✅ | `/dev/ttyN` + getty login + Ctrl+Alt+Fn switching |
| Metal-backed rendering | 🚧 | clear + draw + texture sampling through Metal; vertex/index/uniform in progress (softpipe fallback until done) |
| Full-display takeover | 🚧 | `present_iosurface` flip fires (ret=1); repeatable-frame stability in progress |
| Omarchy rootfs | 🚧 | optional `--rootfs omarchy` (Hyprland + Quickshell) in progress |

## Architecture

```
        Linux guest (Arch, aarch64)
        TTY → Hyprland / KDE → apps
                    │
     syscalls · Wayland · DRM · /dev/snd
                    │
        ┌───────────▼───────────┐
        │   shim (macOS)        │
        │  · syscall translation│
        │  · Wayland server     │
        │  · input / VT / audio │
        │  · Metal present      │
        └───────────┬───────────┘
                    │ IOSurface
        ┌───────────▼───────────┐
        │   rdmetal (driver)    │   ← we wrote this
        │  Mesa Gallium + DRM   │
        └───────────┬───────────┘
                    │
        ┌───────────▼───────────┐
        │   scanout (injection) │   ← we wrote this too
        │  present_iosurface    │
        └───────────────────────┘
```

See [DESIGN.md](DESIGN.md) for the full architecture and the native-machine target.

## Quick start

```sh
git clone --recursive https://github.com/darrinperry83/shimmy.git
cd shimmy
./install.sh                 # builds the shim, bootstraps the Arch rootfs, installs the desktop + rdmetal
./run.sh                     # Hyprland
./run.sh --desktop kde       # KDE Plasma
```

The rootfs is built locally (not stored in git). See [Host setup](README.md#host-setup--sip--the-scanout-injection-one-time) for the one-time SIP + scanout prerequisites.

## Presentation modes

| Mode | Behavior |
|---|---|
| **Windowed** (default) | the desktop runs in a normal window, WindowServer present |
| **Takeover** | suppress WindowServer, `present_iosurface` flips directly to all displays |

## Development

We use a simple trunk-based-on-stable branching model:

- `main` — stable, tagged releases.
- `develop` — staging; where PRs land and CI runs before a release.
- `experimental` — risky/forward work (the scanout takeover, the Metal renderer).

**Workflow:** branch from `develop` → open a PR (CI runs) → review → merge to `develop`.
When a release is cut, `develop` is merged to `main` and tagged. See
[CONTRIBUTING.md](CONTRIBUTING.md) for the full process.

```sh
./build.sh gui     # shim_gui — the display-server host
./build.sh cli     # shim_cli  — headless guest runner
```

## Roadmap

| Item | Priority |
|---|---|
| Repeatable scanout flip (frame 1..N stability) | high |
| Metal vertex/index/uniform (finish the acceleration) | high |
| Native DRM/atomic backend (retire wlsrv presentation) | high |
| Omarchy rootfs (`--rootfs omarchy`) | medium |
| Cross-process dma-buf (accelerated clients) | medium |
| Workspaces across displays | medium |
| System tray + mac integration | medium |
| Chromium `/proc` gap | medium |

## Contributing

Contributions are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) for the workflow and
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for the community standards.

## Security

Report vulnerabilities privately — see [SECURITY.md](SECURITY.md).

## License

[MIT](LICENSE).
