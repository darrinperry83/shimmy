# shimmy

**Run Linux on macOS — as if it were native.**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![shim CI](https://github.com/darrinperry83/shimmy/actions/workflows/ci.yml/badge.svg)](https://github.com/darrinperry83/shimmy/actions)
[![rdmetal build](https://github.com/darrinperry83/shimmy-rdmetal/actions/workflows/build.yml/badge.svg)](https://github.com/darrinperry83/shimmy-rdmetal/actions)
[![PRs welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

shimmy boots a real Arch Linux (aarch64, glibc) userland as a desktop on Apple Silicon macOS.
The graphics are rendered by the **rdmetal** driver — a Mesa Gallium driver we wrote — and
presented by the shim through Metal, with mac apps owned by the compositor as native Wayland
surfaces. No VM. No custom compositor. Just Linux, running like it was installed on the bare
metal.

> **Status: pre-alpha.** It renders Hyprland and KDE Plasma, has working audio and input, but
> there is real work left to harden, accelerate, and polish. This is a development preview,
> not a product.
>
> **Tested on:** Apple M2 Max · 96 GB MacBook Pro · macOS 26.5.1 (arm64e).

---

## Contents

- [Why shimmy](#why-shimmy)
- [Features](#features)
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
| **rdmetal** graphics driver | ✅ | Mesa Gallium + DRM/GBM node, installed as the default GL driver |
| Hyprland desktop | ✅ | stock `hyprland` package, renders + input |
| KDE Plasma desktop | ✅ | stock `plasma-meta`, kwin_wayland + plasmashell |
| Audio | ✅ | PipeWire/ALSA → CoreAudio, auto sink + RUNNING graph |
| mac apps as native windows | ✅ | owned by the compositor as Wayland surfaces |
| TTY / virtual console | 🚧 | `/dev/ttyN` + getty + Ctrl+Alt+Fn (in progress) |
| Full-display takeover | 🚧 | suppress WindowServer, flip directly to all displays |
| Metal-backed rendering | 🚧 | clear+draw done; textures in progress |

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
| Metal-backed GLES (textures → shaders → index buffers) | high |
| TTY/VT console (getty + switching) | high |
| Full-display takeover (dual output + flip) | high |
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
