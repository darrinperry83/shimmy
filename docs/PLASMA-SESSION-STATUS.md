# Plasma session probe (16 November 2026)

The installed Arch root has `/usr/share/wayland-sessions/plasma.desktop` with
`Exec=/usr/lib/plasma-dbus-run-session-if-needed /usr/bin/startplasma-wayland`.
Both executables and `/usr/bin/kwin_wayland` are present. This is package
availability, not evidence that a desktop has started.

`RD_TEST_PLASMA=1` on `tests/run-interactive-login.py` exercises that exact
session command after an isolated systemd → getty → PAM login with a generated
throwaway account. It sets the same empty inherited display variables and
`RDNATIVE_SESSION=1` as `run.sh --init`; it does not modify the installed root.

The private Arch clone reached a login prompt and authenticated UID 22001.
The shell then reported an **empty** `XDG_RUNTIME_DIR`. The packaged Plasma
entry reached `startplasma-wayland` and remained present at the 15-second
snapshot, but no `kwin_wayland` process was seen. The clone's persistent
journal records:

```
pam_systemd(login:session): Failed to issue io.systemd.Login.CreateSession varlink call: Invalid argument
```

The missing logind session and user manager are the first proven blockers;
the underlying Varlink/ABI cause is under separate investigation. Starting
Plasma directly with a hand-made runtime directory reached the same launcher
but did not prove a graphical session, because it bypassed PAM and the user
manager. A previous direct run under the tool sandbox failed D-Bus Unix socket
bind with `EPERM`, which a native host socket probe reproduced: that failure
belongs to the test sandbox, not Shimmy.

This probe does not establish a visible KWin frame, greeter selection, logout,
GPU acceleration, or display takeover. Those require the shared logind/session
fix and a GUI integration run with both Hyprland and Plasma.
