# Boot escape: getting back to macOS

The WindowServer takeover is designed so that **macOS is always reachable**, even if the stand-in,
the keyboard, and the display all fail. The takeover edits the original
`/System/Library/LaunchDaemons/com.apple.WindowServer.plist` **in place** (SIP is disabled), after
backing it up — so recovery is simply restoring that backup.

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

This restores the backed-up original plist over `/System/Library/LaunchDaemons/com.apple.WindowServer.plist`
and removes the stand-in (`/usr/local/sbin/shim_windowserver`) and the boot config
(`/usr/local/etc/shimmy/boot.conf`). The real WindowServer job returns at the next boot.

---

## 2. Single-user mode (Cmd+S)

Hold **Cmd+S** at boot. This boots the main macOS in single-user mode, which skips launchd (and
therefore the edited WindowServer job), dropping to a root shell.

```sh
/sbin/mount -uw /                                        # mount the root filesystem writable
cp '/Library/Application Support/shimmy/com.apple.WindowServer.plist.orig' \
   /System/Library/LaunchDaemons/com.apple.WindowServer.plist
reboot
```

---

## 3. macOS Recovery (Cmd+R)

Hold **Cmd+R** at boot. This boots the separate recovery partition, which is a completely
different macOS install and is immune to anything shimmy did.

From **Utilities -> Terminal**:

```sh
diskutil list                                            # find the main volume (e.g. "Macintosh HD", disk3s1)
diskutil mount disk3s1                                   # mount it at /Volumes/Macintosh HD
cp '/Volumes/Macintosh HD/Library/Application Support/shimmy/com.apple.WindowServer.plist.orig' \
   '/Volumes/Macintosh HD/System/Library/LaunchDaemons/com.apple.WindowServer.plist'
```

Then reboot.

> **Note:** `/System` is normally SIP-protected, but SIP is **disabled** on this machine (which is
> why the takeover can edit the plist in place in the first place). In Recovery the write to
> `/System/Library/LaunchDaemons/...` therefore succeeds. If SIP were re-enabled, remove the backup
> restore and instead reboot into single-user mode with `csrutil disable` from Recovery, or use
> `shimmy release` from a normal boot.

---

## 4. The warm-switch

The stand-in's server loop handles SIGTERM/SIGINT as a clean exit that releases the two
`com.apple.windowserver` receive rights it checked in. If the Linux guest (or the stand-in's own
logic) exits cleanly, the service goes dead and the real WindowServer can re-check-in and take
over. This depends on the stand-in still running well enough to exit cleanly, so the restore
commands above are the dependable path.

---

## The backup

`shimmy takeover` copies the original system plist aside (once) to
`/Library/Application Support/shimmy/com.apple.WindowServer.plist.orig` **before** rewriting it.
The backup is the pristine original, and it is exactly what `shimmy release` (or a manual `cp`)
restores. Keep it; without it the only manual fix is to re-enter the original
`ProgramArguments` (`/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer`
`-daemon`) and drop the added `RDNATIVE_WINDOWSERVER_SYSTEM` environment variable by hand.
