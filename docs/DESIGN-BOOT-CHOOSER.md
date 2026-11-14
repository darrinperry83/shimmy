# Boot chooser — boot macOS or shimmy

The shim runs as an early launchd daemon, **before WindowServer**, and shows a two-option menu.
The choice decides whether macOS boots normally or the shim takes over as the WindowServer
stand-in. macOS is always the safe default.

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

## The menu

Two options, drawn directly by the shim (Metal/framebuffer, no WindowServer needed):

```
        shimmy
    [1] Boot macOS
    [2] Boot Linux (shimmy)

    (defaults to macOS in 10 s)
```

- The chooser reads the keyboard via the HID stack (no WindowServer required).
- A 10-second countdown defaults to **macOS**, so an unattended boot never lands somewhere
  unexpected. Pressing a key cancels the countdown.

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

1. **macOS is the default.** Timeout (and any error) always boots macOS.
2. **Fail-safe watchdog.** If the shim crashes after "shimmy" was chosen, the watchdog relaunches
   the real WindowServer (a few seconds of login-screen flash, then macOS).
3. **Warm-switch back.** The shim's clean exit releases the service and launchd relaunches the
   real WindowServer.
4. **Reversible.** `shimmy takeover` / `shimmy release` flip the arrangement back at any time.

The chooser reuses two things the shim already has: a way to draw (Metal/framebuffer) and a way to
read keys (HID). It is "draw two options, wait for a key, branch."
