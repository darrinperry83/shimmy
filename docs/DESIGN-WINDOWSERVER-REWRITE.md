# WindowServer job rewrite — the stand-in is the WindowServer

`shim_windowserver` becomes the WindowServer by **rewriting the original WindowServer job** in
place. SIP is **disabled** on this machine, so instead of shadowing the job with an `/Library`
override, `shimmy takeover` edits
`/System/Library/LaunchDaemons/com.apple.WindowServer.plist` directly (after backing it up). At the
next boot launchd runs the stand-in as the `com.apple.WindowServer` job, and the stand-in checks in
the real WindowServer service names.

This **replaces** the earlier boot-chooser design (`docs/DESIGN-BOOT-CHOOSER.md`), which is now
superseded: no chooser, no countdown, no exec-fallback. The stand-in *is* the WindowServer.

## What `takeover` changes

`sudo ./bin/shimmy takeover --windowserver /path/to/shim_windowserver` does exactly four things:

1. Backs up the pristine original (once) to
   `/Library/Application Support/shimmy/com.apple.WindowServer.plist.orig`.
2. Rewrites the original's `ProgramArguments` to `["/usr/local/sbin/shim_windowserver"]`.
3. Adds `EnvironmentVariables.RDNATIVE_WINDOWSERVER_SYSTEM = 1`.
4. Installs the stand-in to `/usr/local/sbin/shim_windowserver`.

Everything else in the job is kept **as-is**: `Label` (`com.apple.WindowServer`), the ten
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

Restores the backup over the original plist (`cp` back) and removes the stand-in and
`/usr/local/etc/shimmy/boot.conf`. The real WindowServer job returns at the next boot. Both
commands take effect at the **next** boot — the live WindowServer is never disturbed.

## Risk + recovery

**Risk:** with the job rewritten, the next boot runs the stand-in as the WindowServer. If the
stand-in fails to serve, the display may not come up. Recovery is one `cp`:

- **Cmd+S** (single-user): `cp '/Library/Application Support/shimmy/com.apple.WindowServer.plist.orig' /System/Library/LaunchDaemons/com.apple.WindowServer.plist`
- **Cmd+R** (Recovery): mount the data volume, then the same `cp` under `/Volumes/<volume>/...`.
- Or `sudo ./bin/shimmy release` from a working macOS.

See `docs/BOOT-ESCAPE.md` for the full commands.
