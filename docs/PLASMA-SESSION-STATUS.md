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

## Direct compositor diagnostic

Separately from the login probe, an isolated diagnostic launched
`/usr/bin/kwin_wayland --drm --no-lockscreen` with `LIBSEAT_BACKEND=noop` and a
private `/run/user/22001`. This **bypasses PAM and the user manager**. KWin
accepted `wayland-0`, enumerated two virtual DRM outputs, and flipped black
frames on both. A separate Shimmy process then connected the installed
`/usr/bin/foot` to that socket. KWin's CRTC 1 frames changed to `non-black`
from frame 7; CRTC 2 stayed black with no client on that output. The
`DRM_PRESENT` check samples a coarse grid in the mapped framebuffer. This
establishes that client pixels reach the virtual KMS framebuffer, and the
initial black frames alone were not a demonstrated rendering failure.

A bounded 45-second rerun used `shim_gui` with `RDNATIVE_MODE=windowed` and no
`RDGPU_SCANOUT`. The same nonblack CRTC 1 frames appeared in its log; the GUI
reported the Apple M2 Max Metal device, takeover OFF, and windowed input
capture ON. The user subsequently confirmed seeing the Linux `foot` window
in this diagnostic run, though no full Plasma shell was started. A second
five-minute windowed run repeated the nonblack frame result.

The bounded probe is `tests/probe-plasma-drm-client.py`. Example with a
**disposable** Arch root whose private `/run/user/22001` exists:

```sh
python3 tests/probe-plasma-drm-client.py \
  --root build/plasma-root --gui ./shim_gui --cli ./shim_cli \
  --seconds 45 --logs build/plasma-probe-logs
```

It starts exact KWin and foot processes, uses a fresh AF_UNIX namespace,
disables takeover, disables child core dumps, and tears down only those
processes and its namespace. It does not create the disposable root itself.

The current DRM present path maps the framebuffer's BGRA bytes and calls
`gpuwin_present_crtc` → `present_fb` on the host. This is a CPU pixel handoff
to an AppKit/Metal presenter. The reported Metal device is evidence for that
presenter, **not** proof that KWin's EGL/GLES drawing ran on rdmetal. A direct
`eglinfo -B` diagnostic printed `driver (null)` for virtual PCI 106b:0001 and
did not return a renderer identity before it was stopped. Renderer identity
and fallback counters remain a required GPU gate.

These tests do not establish greeter-selected Plasma startup, a visible
desktop, logout, hardware-backed guest rendering, or display takeover. The
README and desktop plan explicitly say the current live guest still uses
softpipe despite the rdmetal driver being present. The direct diagnostic
also reported DRM magic-authentication and EDID warnings and had no session
D-Bus/PipeWire connection. The shared logind/session fix and full GUI run
remain necessary.
