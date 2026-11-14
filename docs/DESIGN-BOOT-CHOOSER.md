# Boot chooser — boot macOS or shimmy

> **Superseded.** The boot chooser has been dropped in favour of editing the WindowServer job
> directly: `shimmy takeover` rewrites the original `/System/.../com.apple.WindowServer.plist` to
> run the `shim_windowserver` stand-in (no chooser, no countdown, no exec-fallback). See
> [`DESIGN-WINDOWSERVER-REWRITE.md`](DESIGN-WINDOWSERVER-REWRITE.md). This page is kept for
> historical reference only.

The shim runs as an early launchd daemon, **before WindowServer**, and branches on a key press.
The choice decides whether macOS boots normally or the shim takes over as the WindowServer
stand-in. macOS is always the safe default.

The on-screen present (IOMFB + CoreDisplay) is **blocked pre-WindowServer** — the framebuffer
surface returns `kIOReturnNotPrivileged` even as root, and the display id is `0x0` — so the
chooser is **display-less**: it reads a config default and a `/dev/console` key instead of
drawing a menu.

## Boot flow

```
Mac boots -> launchd -> shim-chooser (before WindowServer)
                            |
            +---------------+---------------+
         "macOS"                         "shimmy"
            |                                |
  launch the real WindowServer      register com.apple.windowserver
  -> normal macOS boot              -> Linux environment + mac apps
```

## The keys

No menu is drawn; the chooser waits 10 seconds on the keyboard (HID, with a `/dev/console`
raw-mode fallback):

| Key | Branch |
|---|---|
| `o` `O` `1` `m` `M` (or Enter/Esc) | **macOS** — the escape hatch |
| `2` `l` `L` | shimmy (Linux) |
| anything else | macOS (safe fallback) |
| no key (timeout) | the **config default** |

## The config default and the one-shot

The default boot target (the timeout branch) is read from `/usr/local/etc/shimmy/boot.conf`:

```
default=macOS     # or default=shimmy
once=shimmy       # one-shot: boot shimmy once, then auto-revert to macOS
```

- `default=macOS` (or a missing/unparseable file) → macOS. This is the safe default.
- `default=shimmy` → boot Linux after the timeout. This is **persistent and risky**: every boot
  goes to Linux until you change it back, and the keyboard escape hatch is not reliable at real
  boot, so use `boot once` instead unless you really want Linux every boot. Set it explicitly
  with `sudo ./bin/shimmy boot default shimmy`. `shimmy takeover` never writes it.
- `once=shimmy` → boot shimmy **this boot only**, then the chooser clears the file back to
  `default=macOS` **before** branching. The next boot — whether by reboot or power-button — is
  macOS automatically. This is the **safe way** to try shimmy once: no keyboard, no display,
  no risk of getting stuck in Linux. Set it with `sudo ./bin/shimmy boot once shimmy`.
- `once=macOS` → macOS this boot and clears the file (harmless; same as leaving it at
  `default=macOS`).

`once=` takes precedence over `default=` regardless of line order, and the clear happens before
the branch, so even a crash mid-boot leaves macOS as the next default. The escape hatch holds
regardless of the config: holding `o`/`O` at boot always drops to macOS.

## The branch

- **macOS** — the shim launches the real WindowServer and exits. Normal, untouched macOS.
- **shimmy** — the shim registers `com.apple.windowserver` (+ `.active`) via `bootstrap_check_in`,
  takes over as the WindowServer stand-in, and boots into the Linux environment with mac apps
  inside.

## Launchd ordering

- The chooser is a root launchd daemon in `/Library/LaunchDaemons` (`RunAtLoad`, high priority)
  so it starts before `com.apple.WindowServer`.
- It registers `com.apple.windowserver` in the system bootstrap domain before the real
  WindowServer's own check-in, so the real WindowServer's `bootstrap_check_in` fails and it exits.
- The real WindowServer's binary and plist are never modified; the takeover is purely a service
  race won at boot.

## Safety

1. **macOS is the default.** The config default, a missing/unparseable config, an unknown key,
   and any error all resolve to macOS.
2. **One-shot (the safe escape).** `shimmy boot once shimmy` boots shimmy exactly once and the
   chooser clears the file back to `default=macOS` before branching — so a reboot (or the power
   button) always returns to macOS, with no keyboard or display required. Prefer this over
   `boot default shimmy`.
3. **Escape hatch.** `o`/`O` always forces macOS, regardless of the config, so a machine set to
   boot Linux by default can still be dropped back to macOS at the chooser.
4. **Fail-safe watchdog.** If the shim crashes after "shimmy" was chosen, the watchdog relaunches
   the real WindowServer (a few seconds of login-screen flash, then macOS).
5. **Warm-switch back.** The shim's clean exit releases the service and launchd relaunches the
   real WindowServer.
6. **Reversible.** `shimmy takeover` / `shimmy release` flip the arrangement back at any time.

The chooser reads a key (HID with a `/dev/console` fallback) and a config default, then branches.
It is "wait for a key (or the timeout), then branch."
