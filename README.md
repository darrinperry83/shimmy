# shimmy

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![shim CI](https://github.com/darrinperry83/shimmy/actions/workflows/ci.yml/badge.svg)](https://github.com/darrinperry83/shimmy/actions)
[![rdmetal build](https://github.com/darrinperry83/shimmy-rdmetal/actions/workflows/build.yml/badge.svg)](https://github.com/darrinperry83/shimmy-rdmetal/actions)

Run Linux on macOS — a real Arch Linux (aarch64) desktop rendered by the **rdmetal**
driver and presented on screen by the shim's Metal path, with mac apps owned by the
compositor as native Wayland surfaces.

> **Status: early-stage / pre-alpha.** It boots a real Arch Linux guest and renders the
> Hyprland and KDE Plasma desktops, but there is still a lot to harden, document, and polish.
> Expect rough edges. This is a development preview, not a product.
>
> **Tested on:** Apple M2 Max, 96 GB MacBook Pro, macOS 26.5.1 (arm64e).

## Contents

- [What it does](#what-it-does)
- [Current features](#current-features)
- [Architecture](#architecture)
- [Install](#install-streamlined)
- [Host setup — SIP + the scanout injection](#host-setup--sip--the-scanout-injection-one-time)
- [Development](#development)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

## What it does

shimmy runs an unmodified Arch Linux userland as a desktop on Apple Silicon macOS:

- **No VM, no custom compositor.** The guest is ordinary aarch64 glibc Linux; Hyprland and
  KDE Plasma are the stock Arch packages installed with plain `pacman`.
- **rdmetal is the graphics driver.** A Mesa Gallium driver + a DRM/GBM node present
  `/dev/dri/renderD128` to the guest; the compositor renders through it and the shim presents
  the result via Metal.
- **mac apps are native Wayland windows.** A mac application's window is *created* as a
  `wl_surface`/`xdg_toplevel` in the compositor, so Hyprland/KDE own it (move, resize, focus)
  — not a screen capture.
- **Direct-to-display scanout** (in progress) presents the frame straight to the display,
  bypassing WindowServer's compositing.

## Current features

Working today:

- **Arch Linux rootfs** bootstrapped locally (not stored in git), glibc aarch64.
- **Hyprland** desktop — stock `hyprland` package, renders, foot terminal, cursor.
- **KDE Plasma** desktop — stock `plasma-meta`, kwin_wayland + plasmashell render.
- **rdmetal driver** installed as the default GL driver (no env vars needed).
- **mac apps owned by the compositor** — window creation + ownership wiring; real pixels
  via the app's IOSurface (foreign-app pixels still gated by macOS TCC).
- **PipeWire / ALSA → CoreAudio** — a synthetic ALSA card + host AudioUnit; WirePlumber
  auto-creates the sink and `pw-cat` drives a RUNNING graph into CoreAudio.
- **Direct scanout** — injection payload built (present flips), final wiring in progress.

## Architecture

```
macOS host
├── shim (this repo)         syscall translation, Wayland server (wlsrv.c), input,
│                            Metal present, mac-window→Wayland-surface bridge
├── rdmetal/ (submodule)     Mesa Gallium driver + DRM/GBM node
└── scanout/ (submodule)     WindowServer injection (direct-to-display present)

Linux guest (Arch, aarch64)   Hyprland / KDE Plasma, standard pacman packages
```

## Install (streamlined)

```sh
git clone --recursive https://github.com/darrinperry83/shimmy.git
cd shimmy
./install.sh                 # builds the shim, bootstraps the Arch rootfs, installs the desktop + rdmetal
./run.sh                     # boots Hyprland  (or: ./run.sh --desktop kde)
```

`install.sh` produces `rootfs/` locally (the rootfs is **not** committed), installs the
desktop with the standard Arch commands (`pacman -S hyprland` / `pacman -S plasma`), and
installs rdmetal as the default GL driver.

## Host setup — SIP + the scanout injection (one-time)

The direct-to-display scanout injects a small payload into WindowServer. That needs two
one-time host prerequisites, fully automated by `./setup.sh`:

### 1. Disable System Integrity Protection (SIP)

The injector uses `task_for_pid(WindowServer)` to hand a Mach receive-right into
WindowServer, which macOS blocks while SIP is on.

1. Shut down, then boot into **macOS Recovery**:
   - Apple silicon: hold the power button until "Loading startup options", choose Options → Continue.
2. In Recovery, open **Terminal** (Utilities menu) and run:
   ```sh
   csrutil disable
   ```
3. Reboot normally.

> Re-enable later with `csrutil enable` in Recovery. This is a real security trade-off —
> only do it if you accept it.

### 2. Passwordless sudo for the injector

The injector must run as root. Add a single NOPASSWD rule (substitute your user and the
repo path):

```sh
sudo visudo
# add:
#   <youruser> ALL=(ALL) NOPASSWD: /path/to/shimmy/scanout/ws_inject_manual_d2
```

### 3. Inject (and after every WindowServer restart)

`./setup.sh` does the SIP check, verifies the sudo rule, and injects the payload for the
current macOS version:

```sh
./setup.sh
# equivalent:
#   sudo ./scanout/ws_inject_manual_d2 --register ./scanout/payload/rdgpu-scanout-server18-payload
```

WindowServer restarts (login, `sudo killall WindowServer`, a crash) clear the injection, so
re-run `./setup.sh` after any of those. The payload is pinned to an exact macOS version —
see `scanout/` for the tags.

## Development

Build the shim:

```sh
./build.sh gui     # shim_gui — the display-server host
./build.sh cli     # shim_cli  — headless guest runner
```

The driver and the injection are separate submodules:

- `rdmetal/` — build the Mesa driver against Mesa 24.0.9 (see its README).
- `scanout/` — the WindowServer injection, tagged per macOS version.

The full working step-by-step (the reverse-engineering and the migration notes) is in
`ARCH_MIGRATION.md` and the `scanout/SKYLIGHT_SCANOUT_*.md` notes.

## Roadmap

- [ ] Metal-backed GLES rendering for rdmetal (currently CPU/softpipe).
- [ ] Direct-scanout flip landed and stable.
- [ ] System tray with mac integration.
- [ ] Foreign mac-app pixels (TCC/entitlement path).

## Contributing

Contributions are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) and
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Report bugs and request features via the issue
templates. Security issues should be reported privately per [SECURITY.md](SECURITY.md).

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## License

[MIT](LICENSE).
