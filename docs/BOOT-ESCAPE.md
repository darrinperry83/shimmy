# Boot escape: getting back to macOS

The WindowServer takeover is designed so that **macOS is always reachable**, even if the stand-in,
the keyboard, and the display all fail. The takeover does **not** edit `/System` — `/System` is the
sealed system volume and is read-only even with SIP disabled. Instead it installs an **override** at

    /Library/LaunchDaemons/com.apple.WindowServer.plist

(the writable data volume) with the same `Label` (`com.apple.WindowServer`). launchd gives the
`/Library` copy precedence over the untouched `/System` copy, so the stand-in runs as the
WindowServer job. Recovery is therefore a single `rm` of that override — launchd then falls back to
`/System` and the real WindowServer returns.

The escapes, in order of preference:

1. `shimmy release` (from a working macOS).
2. Single-user mode (Cmd+S).
3. macOS Recovery (Cmd+R).
4. The stand-in's warm-switch (clean exit releases the service).

---

## 1. `shimmy release` (the normal escape)

From a working macOS (or any shell where the repo is reachable):

```sh
sudo ./bin/shimmy release
```

This removes the override (`/Library/LaunchDaemons/com.apple.WindowServer.plist`), the stand-in
(`/usr/local/sbin/shim_windowserver`), and the boot config (`/usr/local/etc/shimmy/boot.conf`).
The real WindowServer job returns at the next boot.

---

## 2. Single-user mode (Cmd+S)

Hold **Cmd+S** at boot. This boots the main macOS in single-user mode, which skips launchd (and
therefore the WindowServer override), dropping to a root shell.

```sh
/sbin/mount -uw /                                        # make the data volume writable (if needed)
rm /Library/LaunchDaemons/com.apple.WindowServer.plist   # drop the override; launchd falls back to /System
reboot
```

---

## 3. macOS Recovery (Cmd+R)

Hold **Cmd+R** at boot. This boots the separate recovery partition, which is a completely
different macOS install and is immune to anything shimmy did.

From **Utilities -> Terminal**:

```sh
diskutil list                                            # find the Data volume (e.g. "Macintosh HD - Data")
diskutil mount "Macintosh HD - Data"                     # mount it under /Volumes
rm '/Volumes/Macintosh HD - Data/Library/LaunchDaemons/com.apple.WindowServer.plist'
```

Then reboot. Removing the override is enough: launchd falls back to the untouched
`/System/Library/LaunchDaemons/com.apple.WindowServer.plist`.

> **Note:** the override lives on the **data volume**, which is writable — no `csrutil` / SIP step
> is needed to remove it. `/System` itself is never modified by shimmy, so there is nothing to
> restore there.

---

## 4. The warm-switch

The stand-in's server loop handles SIGTERM/SIGINT as a clean exit that releases the two
`com.apple.windowserver` receive rights it checked in. If the Linux guest (or the stand-in's own
logic) exits cleanly, the service goes dead and the real WindowServer can re-check-in and take
over. This depends on the stand-in still running well enough to exit cleanly, so the `rm` / release
above are the dependable path.

---

## The reference backup

`shimmy takeover` copies the original `/System` plist aside (once) to

    /Library/Application Support/shimmy/com.apple.WindowServer.plist.orig

This is for **reference only** — it is the pristine original job definition, so you can see exactly
what the real WindowServer looked like (its `ProgramArguments`
`/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer -daemon`, its
`MachServices`, `POSIXSpawnType`, and `_Conclave`). Recovery never needs it: `/System` is untouched,
and removing the override is the entire recovery.
