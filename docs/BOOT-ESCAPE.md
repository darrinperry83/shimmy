# Boot escape: getting back to macOS

The boot chooser is designed so that **macOS is always reachable**, even if shimmy, the
keyboard, and the display all fail. This page documents every way back.

The four escapes, in order of preference:

1. Reboot (the one-shot auto-revert).
2. Single-user mode (Cmd+S).
3. macOS Recovery (Cmd+R).
4. The crash watchdog.

---

## 1. The one-shot (the safe escape)

Do NOT use `boot default shimmy` for everyday booting (it is persistent and needs a working
keyboard override to escape). Use the one-shot instead:

```sh
sudo ./bin/shimmy boot once shimmy
```

The chooser boots shimmy **once** and, before branching, rewrites the boot config back to
`default=macOS`. So the next boot is always macOS.

| Action | Result |
|---|---|
| `boot once shimmy` | next boot = shimmy, then auto-reverts to macOS |
| reboot (or hold power) | macOS |
| `boot default shimmy` | persistent shimmy (the risky option) |
| `boot default macOS` | persistent macOS |

The one-shot is consumed *before* the branch is taken (clear-before-branch), so even a crash
mid-shimmy-boot leaves macOS as the next default.

---

## 2. Single-user mode (Cmd+S)

Hold **Cmd+S** at boot. This boots the main macOS in single-user mode, which skips launchd
(and therefore the chooser), dropping to a root shell.

```sh
/sbin/mount -uw /                                        # mount the root filesystem writable
printf 'default=macOS\n' > /usr/local/etc/shimmy/boot.conf
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
printf 'default=macOS\n' > '/Volumes/Macintosh HD/usr/local/etc/shimmy/boot.conf'
```

Then reboot. `/usr/local/etc/` is not SIP-protected, so the recovery Terminal (running as root)
writes it without issue.

---

## 4. The crash watchdog

The chooser waits on the shimmy stand-in. If it exits uncleanly (a crash or a signal), the
chooser relaunches the real WindowServer and hands back to macOS. This is automatic and does not
depend on any input.

---

## The config file

The chooser reads `/usr/local/etc/shimmy/boot.conf` at boot. Recognized forms:

```ini
default=macOS     # persistent macOS
default=shimmy    # persistent shimmy (risky)
once=shimmy       # boot shimmy once, then auto-revert to macOS
once=macOS        # boot macOS once (harmless)
```

Missing or unparseable file means macOS.
