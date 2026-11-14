# shimmy configuration

shimmy is declarative: **one file defines the whole Linux machine**, and the machine is a
pure function of that file.

    machine = f(shimmy.nix, shimmy.lock)

`shimmy.nix` is the human-edited config (the NixOS `configuration.nix` model). `shimmy.lock`
pins the inputs — the Arch snapshot, the rdmetal driver, the shim revision, and the macOS
substrate — so the same two files always rebuild the same machine. You never hand-edit the lock.

The config has six fields. Everything else is derived from them: which displays Linux takes
over, which compositor launches, which packages get installed, and which hotkey flicks between
the two OSes. See `config/shimmy.nix.example` for a copy-and-paste starting point.

## Fields

| Field | Type | Default | Meaning |
|---|---|---|---|
| `mode` | string | `"windowed"` | how Linux and macOS share the hardware |
| `displays` | list of strings | `[ "external" ]` | which displays Linux takes over |
| `rootfs` | string | `"arch"` | the Linux userland ("base system") profile |
| `windowManager` | string | `"hyprland"` | the desktop / compositor to launch |
| `apps` | list of strings | `[ ]` | the application set, installed via `pacman` |
| `flick.key` | string | `"ctrl-alt-f"` | the hotkey that flicks between Linux and macOS |

## `mode`

The single most important knob. The Linux compositor always runs as the DRM master against the
rdmetal node; `mode` selects the **present sink** — how the finished frame is shown and which
side of the hardware macOS keeps.

| Value | macOS side | Linux side | Present sink |
|---|---|---|---|
| `windowed` | full macOS | Linux in a resizable macOS window | `CAMetalLayer` |
| `single-display` | the other displays | the taken display, full desktop | `present_iosurface` |
| `kiosk` | the other displays | one locked app on the taken display | `present_iosurface` |
| `all-displays` | suppressed | every display, full desktop | `present_iosurface` |

`windowed` is the safe default. In `windowed`, `displays` is ignored: the Linux display is the
window itself.

## `displays`

Which physical displays Linux takes over. Relevant only when `mode` is not `windowed`.

Accepts symbolic names (`"external"`, `"builtin"`, `"all"`) or a specific display id/name. The
taken-over displays are removed from macOS (a virtual unplug) and flip frames directly to the
display controller; the remaining displays stay macOS. With `mode = "all-displays"` this is
normally `[ "all" ]`.

## `rootfs`

The Linux userland profile — the "base system" the machine boots, in the NixOS sense of the
base system a module set produces.

| Value | Description |
|---|---|
| `arch` | bare Arch aarch64 + glibc; you install everything yourself. |
| `omarchy` | Arch + Hyprland + Quickshell + a curated bar, pre-configured. |
| `kiosk` | a minimal rootfs that boots straight into one locked app — no shell, nothing to escape to. |

A profile is a declarative module that consumes `windowManager` + `apps` and produces the
rootfs. Choosing a new profile is choosing a different base system, not a different code path.

## `windowManager`

The desktop / compositor to run. Installed via `pacman`; the field only *names* it — the runner
derives the launch command (`hyprland` → `Hyprland`, `plasma` → `startplasma-wayland`,
`sway` → `sway`, …). Any window manager in the Arch aarch64 repos is fair game; the shim does
not special-case it beyond the launch mapping.

## `apps`

The application set, installed via `pacman` from the full Arch aarch64 repos. The package
manager is `pacman`; this list is a *declaration of intent* — the set of packages the machine
wants, so the whole thing stays reproducible.

In `mode = "kiosk"` (paired with `rootfs = "kiosk"`), the single entry is the locked app: it is
started full-screen with no shell and no way to escape it.

## `flick.key`

The hotkey that switches between Linux and macOS — the product's most important interaction,
expressed as one gesture everywhere:

- **Single screen** — the key is a *toggle*: it flips the whole screen between Linux and macOS.
- **Multiple screens** — the key is a *swap*: it moves the taken-over set (which screen is Linux
  is already `displays`), or the pointer crosses the screen boundary.

The flip is near-instant because macOS is never killed: suppression is a reversible flag, and
both OSes keep running underneath, so flicking feels like switching full-screen spaces, not
rebooting.

The value is a modifier-plus-key combo (`"ctrl-alt-f"`). Only `flick.key` is required today; it
is namespaced under `flick` so future scoping options (e.g. `flick.on`) slot in without a schema
break.

## The machine is a function of the config

The six fields compose into one system. For example:

```nix
{
  mode          = "single-display";           # external is Linux, builtin stays macOS
  displays      = [ "external" ];
  rootfs        = "omarchy";                  # pre-configured Hyprland base
  windowManager = "hyprland";
  apps          = [ "foot" "chromium" "neovim" ];
  flick.key     = "ctrl-alt-f";
}
```

means: boot the Omarchy rootfs, launch Hyprland as the DRM master on the external display while
macOS keeps the builtin, install `foot`, `chromium`, and `neovim` from the Arch repos, and let
`Ctrl+Alt+F` flick between the two. Change any field, re-run the rebuild, and the machine is
updated to match — nothing else is touched by hand.
