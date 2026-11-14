# Boot chooser — boot macOS or shimmy

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

## The config default

The default boot target (the timeout branch) is read from `/usr/local/etc/shimmy/boot.conf`:

```
default=macOS     # or default=shimmy
```

- `default=macOS` (or a missing/unparseable file) → macOS. This is the safe default.
- `default=shimmy` → boot Linux after the timeout, but only after the user explicitly runs
  `sudo ./bin/shimmy boot default shimmy`. `shimmy takeover` never writes it.

The escape hatch holds regardless of the config default: holding `o`/`O` at boot always drops to
macOS.

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
2. **Escape hatch.** `o`/`O` always forces macOS, regardless of the config default, so a machine
   set to boot Linux by default can still be dropped back to macOS at the chooser.
3. **Fail-safe watchdog.** If the shim crashes after "shimmy" was chosen, the watchdog relaunches
   the real WindowServer (a few seconds of login-screen flash, then macOS).
4. **Warm-switch back.** The shim's clean exit releases the service and launchd relaunches the
   real WindowServer.
5. **Reversible.** `shimmy takeover` / `shimmy release` flip the arrangement back at any time.

The chooser reads a key (HID with a `/dev/console` fallback) and a config default, then branches.
It is "wait for a key (or the timeout), then branch."
