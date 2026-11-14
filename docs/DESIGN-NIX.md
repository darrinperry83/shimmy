# shimmy as a declarative system

> A design proposal for making shimmy feel like Nix: one file defines the whole Linux machine,
> the machine is a pure function of that file, and macOS is just the hardware layer underneath.

## The pitch

shimmy's product identity is: **Linux is the primary experience, macOS is the hardware layer
underneath.** When you flick into Linux, you are on a Linux machine; when you flick back, you are
on macOS. The shim is the thin bridge that makes both real.

The way to make that feel native, reproducible, and *owned* is to express the whole thing in
**one declarative file** — the NixOS `configuration.nix` model — and derive everything else from
it: the mode, which displays Linux takes over, the rootfs profile, the window manager, the app
set, the flick key.

NixOS's insight is that a system is a pure function of its configuration plus pinned inputs.
shimmy can borrow that insight wholesale, with one honest difference: **the hardware (the CPU, GPU,
audio, display) is Apple's and stays closed.** shimmy is declarative about everything *above* that
line — the Linux userland, the compositor, the apps, and how the two OSes share the screens — and
it treats macOS's scheduler and drivers the way NixOS treats the kernel: a fixed, pinned substrate.

## 1. The config: `shimmy.nix`

One file, checked into a repo, defines the whole machine. The schema below is the target; fields
marked with a `*` are the first cut.

```nix
# shimmy.nix — the whole Linux machine, in one file
{
  # ---- the Linux userland ----

  # Which rootfs profile to boot. This is the "base system" of the machine.
  #   "arch"     — bare Arch aarch64 + glibc, nothing else. You install everything.
  #   "omarchy"  — Arch + Hyprland + Quickshell + a curated bar, pre-configured.
  #   "kiosk"    — a minimal rootfs that boots straight into one locked app.
  rootfs = "omarchy";

  # The window manager / desktop. Installed via pacman; this only names it.
  # The runner derives the launch command from this (hyprland -> Hyprland,
  # plasma -> startplasma-wayland, sway -> sway, ...).
  windowManager = "hyprland";

  # The app set, installed via pacman (the full Arch aarch64 repo).
  packages = [ "foot" "chromium" "neovim" "firefox" "wireguard-tools" ];

  # System services to enable (guest-side, systemd units).
  services = [ "pipewire" "wireplumber" "sshd" ];

  # Apps/services to auto-start on the desktop.
  autostart = [ "foot" "waybar" ];

  # ---- how Linux and macOS share the hardware ----

  # The mode. This is the single most important knob.
  #   "windowed"       — Linux in a window (one display), macOS fully usable.
  #   "single-display" — one display becomes Linux, the other stays macOS.
  #   "kiosk"          — one display runs one locked app, the other stays macOS.
  #   "all-displays"   — every display becomes Linux, macOS suppressed.
  mode = "single-display";

  # Which displays Linux takes over (ignored in "windowed").
  # Accepts symbolic names ("external", "builtin", "all") or a display id/name.
  takeover.displays = [ "external" ];

  # The flick: how you switch between Linux and macOS.
  #   key   — the hotkey (the canonical gesture, works on one or many screens).
  #   on    — where the flick applies. "all" = every display; "taken" = only the
  #           taken-over displays (so macOS-only screens are never interrupted).
  flick.key = "ctrl-alt-f";
  flick.on  = "all";

  # ---- the kiosk app (only when mode == "kiosk") ----
  kiosk = {
    app = "chromium";                 # the single locked app
    args = [ "--kiosk" "https://dashboard.internal" ];
    # No shell, no switching, nothing to escape to.
  };

  # ---- misc ----
  # Environment variables passed into the guest (RDENV_* passthrough).
  env = {
    LANG = "en_AU.UTF-8";
  };

  # The user to auto-login at the TTY (omit to drop to a getty login).
  user = "darrin";
}
```

The system is a function of this file: `machine = f(shimmy.nix, shimmy.lock)`. Change the config,
re-run one command, and the machine is rebuilt to match. Nothing else is touched by hand.

### Why a Nix-style file and not YAML/TOML

Nix gives three things a flat format can't: **functions** (share a `takeover.displays` expression
between two machines), **conditionals** (`kiosk = if mode == "kiosk" then {...} else null`), and
**imports** (split the config into `hosts/work.nix` + `common.nix`). Those are exactly the things
that make a config a *system definition* rather than a settings dump.

The tradeoff is the Nix toolchain on macOS. The proposal is to start with a **Nix-flavored schema
evaluated by a small, self-contained evaluator** (a strict subset of the Nix expression language),
and grow it toward real Nix if the project wants the full ecosystem later. A YAML/TOML schema is
the fallback if a hand-rolled evaluator is too much — but it loses the "system as an expression"
quality that makes Nix feel like Nix.

## 2. The mode / flick model

The four modes are not four products; they are four **present sinks** over the same engine. The
Linux compositor always runs as the DRM master against the rdmetal node. What changes is how its
flip is presented:

| Mode | macOS side | Linux side | present sink |
|---|---|---|---|
| `windowed` | full macOS | one display in a window | `CAMetalLayer` |
| `single-display` | the other display | the taken display, full | `present_iosurface` |
| `kiosk` | the other display | one locked app, full | `present_iosurface` |
| `all-displays` | suppressed | every display, full | `present_iosurface` |

**The flick** is the product's most important interaction, and it is *one gesture* everywhere:

- **Multiple screens** — the flick is **spatial**: the pointer (and keyboard focus) crosses the
  screen boundary, or the key moves the taken-over set. macOS and Linux are side by side.
- **Single screen** — the flick is a **toggle**: the key flips the whole screen between Linux and
  macOS. Both OSes keep running underneath; the flip is near-instant because suppression is a
  reversible flag, not a teardown.

The config's `flick.key` is the canonical gesture; `flick.on` scopes it. The spatial flick needs
no config beyond the mode, because "which screen is Linux" is already `takeover.displays`.

## 3. The reproducibility story: `shimmy.lock`

"Same config → same Linux machine" is only true if the *inputs* are pinned too. NixOS pins them in
`flake.lock`; shimmy's equivalent is `shimmy.lock`, which pins the four things that determine the
built machine:

```jsonc
// shimmy.lock — generated, never hand-edited. Pins the machine's inputs.
{
  "version": 1,

  // the Arch snapshot the rootfs is built from (pacman mirror + package DB)
  "arch": {
    "mirror": "https://geo.mirror.pkgbuild.com",
    "snapshot": "2026-11-14T00:00:00Z",      // a pacman archive snapshot
    "keyring": "archlinux-keyring@20261101"   // the keyring that signed it
  },

  // the rdmetal driver (a git pin; the default GL driver is built from this)
  "driver": {
    "repo": "github.com/darrinperry83/shimmy-rdmetal",
    "rev": "9c1f…",
    "mesa": "24.3.0"                          // the Mesa release it patches
  },

  // the shim itself (the host-side bridge + the scanout offsets)
  "shim": {
    "repo": "github.com/darrinperry83/shimmy",
    "rev": "6c0f7bb…"
  },

  // the macOS substrate the scanout offsets were derived against.
  // This is the honest "closed hardware" pin: the takeover reverse-engineering
  // is version-specific, so it is pinned like a kernel ABI.
  "macos": "26.5.1 (25F80)"
}
```

With both files, the build is deterministic:

1. `shimmy build` → rebuilds the rootfs from the pinned Arch snapshot, builds the pinned driver,
   installs it as the default GL driver, and lays down the packages from `packages` + `services`.
2. `shimmy switch` → applies the config's mode/displays/flick (the *runtime* half, which does not
   require a rootfs rebuild).
3. `shimmy run` → boots the machine from the config.

This gives the Nix split for free: **build vs. switch vs. run** — the expensive part (the rootfs +
driver) is a cacheable, pinned build; the cheap part (the mode, the displays, the flick) is a
runtime flag change. Changing only `flick.key` is a `switch`, not a `build`.

The lockfile is generated (not hand-written) the way `flake.lock` is, and `shimmy update` bumps the
pins. Reproducibility = "re-run `shimmy build` from the same two files, get the same machine."

## 4. The package story

There is no shimmy package manager. **The package manager is `pacman`, against the full Arch aarch64
repos.** The config's `packages` list is a *declaration of intent*, not a replacement for pacman:

- `pacman -S hyprland` installs a WM; the config's `windowManager = "hyprland"` just names it.
- `rdmetal` is not a package you opt into — it is installed as the **default GL driver** (zero env
  vars), so every standard GL/EGL app gets Metal through Mesa with no configuration.
- The `kiosk` profile is a rootfs that ships *one* app and no shell — it is a packaging choice, not
  a runtime flag.

This is the honest way to get "all the ARM64 packages you can get your hands on": the machine is a
stock Arch aarch64 userland, so the entire ecosystem just works; the config only *names* which of
those packages the machine wants, so the whole thing is reproducible.

## 5. What to build to make it "feel like NIX"

In dependency order:

1. **`shimmy.nix` schema + a small evaluator.** The config file, a schema validator, and a
   strict-subset Nix evaluator (or the YAML/TOML fallback). This is the foundation; everything
   else is a consumer of the evaluated config.
2. **`shimmy.lock` + the reproducible rootfs build.** The lockfile format, the `shimmy build` that
   rebuilds the rootfs from the pinned Arch snapshot + driver + shim, and a content-addressed
   cache so a rebuild with an unchanged lock is a no-op.
3. **The three commands.** `shimmy build` (rootfs + driver), `shimmy switch` (mode/displays/flick
   only), `shimmy run` (boot). These are the "feel like Nix" surface — one-word, idempotent,
   config-driven.
4. **The rootfs profiles as modules.** `arch`, `omarchy`, `kiosk` become declarative modules that
   take the config's `packages`/`services`/`windowManager` and produce the rootfs. A new profile
   is a new module, not a new code path.
5. **The mode/display/flick derivation.** Map the config's `mode` + `takeover.displays` + `flick`
   onto the runtime flags the shim already understands (`RDGPU_SCANOUT`, the display ids, the
   present sink). This is the "switch" half, and it should be a pure function of the config.
6. **The flick as a first-class input.** A single hotkey that works on one or many screens, driven
   by `flick.key`/`flick.on`, and wired to the reversible macOS suppression so the flip is instant.

The end state: a user clones a repo with `shimmy.nix` + `shimmy.lock`, runs `shimmy build && shimmy
switch && shimmy run`, and gets their exact Linux machine — the WM they named, the apps they listed,
on the display they chose, with the flick they configured — and macOS is just the layer underneath,
one keypress away.

## Open questions (to decide, not block)

1. **Nix toolchain vs. a hand-rolled evaluator.** Full Nix on macOS is a real dependency; a strict
   subset is lighter but a maintenance burden. Lean: start with the subset, grow to Nix.
2. **`shimmy switch` live-apply vs. require a restart.** The takeover display change may need a
   WindowServer re-inject (a restart) — decide whether `switch` is live or schedules the change.
3. **The lockfile's macOS pin.** Pin just the version, or also the shared-cache offsets (the
   slide-sensitive constants)? The honest answer is the offsets, since they drift per boot.
