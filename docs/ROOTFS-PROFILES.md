# Rootfs profiles

A **rootfs profile** is the "base system" of the Linux machine: what ships in the guest filesystem
before you log in and start installing things. Every profile is built from the stock **Arch Linux
ARM aarch64** rootfs tarball, bootstrapped locally, and is **not stored in git** — the built
`rootfs/` directory is a local artifact.

There are exactly two rootfs choices today, `arch` and `omarchy`, with `kiosk` planned:

| Profile | Status | Contents |
|---|---|---|
| `arch` | ✅ default | bare Arch aarch64 + glibc, nothing else |
| `omarchy` | ✅ | Arch + Hyprland + Quickshell + a curated bar, pre-configured |
| `kiosk` | 📋 planned | a minimal rootfs that boots straight into one locked app |

## `arch` — the bare base

The default profile. A stock Arch Linux ARM aarch64 userland with glibc and **nothing else**
preinstalled: no desktop, no window manager, no curated apps. It boots to a TTY login, and from
there the entire Arch aarch64 package ecosystem is yours through `pacman`.

`arch` is the honest "base system" — you install everything you want, and the shim stays out of
your way.

```sh
./install.sh        # arch (the default) — boots to the TTY login
```

## `omarchy` — Arch + Hyprland + Quickshell

The **Omarchy** profile is `arch` plus a pre-configured desktop stack: the **Hyprland** tiling
compositor, **Quickshell** (the Wayland shell/bar), and a curated bar layout, all wired up so it
boots into a working desktop out of the box. It is the one profile that ships a desktop
pre-configured rather than leaving it to the user.

```sh
./install.sh --rootfs omarchy    # Arch + Hyprland + Quickshell, pre-configured
```

Omarchy is the "opinionated desktop" profile. It is a convenience: the same result is reachable
from `arch` by `pacman -S hyprland quickshell` plus a Hyprland/Quickshell config, but `omarchy`
does that for you.

## `kiosk` — minimal, one locked app (planned)

A **minimal rootfs that boots straight into one locked application**: no shell, no switching,
nothing to escape to. It is the single-display takeover's rootfs counterpart — a packaging choice,
not a runtime flag — for dashboards, signage, or point-of-sale terminals. macOS keeps the
operator's display.

`kiosk` is on the roadmap (📋). It ships the single app *inside the rootfs* and leaves the shell
out entirely, which is why it is a profile rather than a launch option.

## The install model

The rootfs choice is **only `arch` or `omarchy`** (plus `kiosk` planned). There is no third option,
and there is no `--desktop` flag on the profile: **the desktop is always installed by you through
`pacman`.**

```sh
./install.sh                    # arch — the bare base, boots to the TTY
./install.sh --rootfs omarchy   # omarchy — Hyprland + Quickshell, pre-configured
```

Inside the guest, install the desktop of your choice through the package manager:

```sh
pacman -S hyprland    # or plasma, sway, i3, gnome, ...
```

The profile only selects the base system. `omarchy` is the sole exception that pre-ships a
specific desktop (Hyprland + Quickshell); for anything else — KDE Plasma, Sway, GNOME, i3, or any
other package in the Arch repos — you start from `arch` and install it with `pacman`. `rdmetal` is
not part of any profile choice either: it is installed as the **default GL driver** (zero env
vars), so every standard GL/EGL app gets Metal through Mesa regardless of profile.

## How this maps forward

The same three names become the `rootfs` field of the declarative `shimmy.nix` config, and the
`pacman`-installs-the-desktop rule becomes its `windowManager` + `packages` fields — the config
*names* the desktop and apps, `pacman` installs them. See
[DESIGN-NIX.md](DESIGN-NIX.md) for that proposal.
