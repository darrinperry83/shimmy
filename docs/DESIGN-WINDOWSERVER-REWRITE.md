# WindowServer override — the stand-in is the WindowServer

`shim_windowserver` becomes the WindowServer by installing an **override** at
`/Library/LaunchDaemons/com.apple.WindowServer.plist` (the writable data volume) with the same
`Label` (`com.apple.WindowServer`). launchd gives the `/Library` copy precedence over the untouched
`/System/Library/LaunchDaemons/com.apple.WindowServer.plist`, so at the next boot launchd runs the
stand-in as the `com.apple.WindowServer` job, and the stand-in checks in the real WindowServer
service names.

`/System` is the sealed system volume (read-only even with SIP disabled), so the earlier in-place
edit of `/System/.../com.apple.WindowServer.plist` was **not** viable; the override approach writes
only to `/Library` and never touches `/System`.

This **replaces** the earlier boot-chooser design (`docs/DESIGN-BOOT-CHOOSER.md`), which is now
superseded: no chooser, no countdown, no exec-fallback. The stand-in *is* the WindowServer.

## What `takeover` changes

`sudo ./bin/shimmy takeover --windowserver /path/to/shim_windowserver` does exactly four things:

1. Reads the pristine `/System` plist (read-only) and backs it up (once) to
   `/Library/Application Support/shimmy/com.apple.WindowServer.plist.orig` — for **reference only**.
2. Writes the override to `/Library/LaunchDaemons/com.apple.WindowServer.plist` with
   `ProgramArguments` = `["/usr/local/sbin/shim_windowserver"]`.
3. Adds `EnvironmentVariables.RDNATIVE_WINDOWSERVER_SYSTEM = 1`.
4. Installs the stand-in to `/usr/local/sbin/shim_windowserver`.

Everything else in the job is copied **verbatim**: `Label` (`com.apple.WindowServer`), the ten
`MachServices` (including `com.apple.windowserver` and `com.apple.windowserver.active`),
`POSIXSpawnType` (`Interactive`), and `_Conclave`.

## The stand-in check-in

At startup the stand-in's `ws_bootstrap_check_in_system()` (`src/windowserver.c` in the
`shim-metal-clear` tree) does `bootstrap_check_in("com.apple.windowserver")` then
`bootstrap_check_in("com.apple.windowserver.active")`, gated on `RDNATIVE_WINDOWSERVER_SYSTEM=1`
and root, then serves the ~37 SLS functions in its receive loop. On a clean exit it releases the
two receive rights (the warm-switch), after which the real WindowServer can re-check-in.

## `release`

```sh
sudo ./bin/shimmy release
```

Removes the override (`/Library/LaunchDaemons/com.apple.WindowServer.plist`), the stand-in, and
`/usr/local/etc/shimmy/boot.conf`. launchd then falls back to the untouched `/System` plist, so the
real WindowServer job returns at the next boot. Both commands take effect at the **next** boot —
the live WindowServer is never disturbed.

## Risk + recovery

**Risk:** with the override in place, the next boot runs the stand-in as the WindowServer. If the
stand-in fails to serve, the display may not come up. Recovery is one `rm`:

- **Cmd+S** (single-user): `rm /Library/LaunchDaemons/com.apple.WindowServer.plist`
- **Cmd+R** (Recovery): mount the data volume, then the same `rm` under `/Volumes/<volume>/...`.
- Or `sudo ./bin/shimmy release` from a working macOS.

`/System` is never modified, so there is nothing to restore there; the reference backup
(`/Library/Application Support/shimmy/com.apple.WindowServer.plist.orig`) is kept only so the
original job definition can be inspected.

See `docs/BOOT-ESCAPE.md` for the full commands.
