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

## Authenticated Plasma and Plasmashell JIT diagnostic (2026-11-16)

In a disposable Arch root, real systemd boot, getty/password login as a
throwaway UID 22001, and the installed `startplasma-wayland` entry now start
KWin and a Wayland `foot` client. In a bounded 85-second windowed GUI probe,
KWin and foot kept the same PIDs and the builtin virtual CRTC presented
nonblack frames. This does not establish a fully working Plasma session or
hardware-backed guest rendering.

`plasma-plasmashell.service` is a D-Bus-name service with a 40-second start
timeout. Without a workaround, its process faults before owning
`org.kde.plasmashell`: `libpcre2-16.so.0.15.0` branches into anonymous JIT
memory at page offset `+0x10`, where the instruction is `00000000`.
Repeated service timeouts follow that process failure.

A **clone-only diagnostic** `QT_ENABLE_REGEXP_JIT=0` for the Plasmashell unit
eliminated the fault in the bounded GUI run: KWin, foot, and Plasmashell kept
their PIDs, and systemd logged `Started KDE Plasma Workspace`. This was not
added to the product. A simple standalone `pcre2test -16 -jit` succeeded,
so the fault depends on the actual Plasmashell JIT path.

A second clone-only test carried `RDNOJITREWRITE=1` across guest exec and
traced JIT bytes before and after the skipped rewrite. The same Plasmashell
fault occurred twice. On the faulting page, the first executable flip had
valid ARM64 words at `+0x10` (`a9b97bfd,a90653f3,a9055bf5,f90027f7`);
before a later executable flip the same words were all zero, and skipping
the rewrite left them zero. No overlapping guest `mmap(MAP_FIXED)`, `munmap`,
or `mprotect`, nor a logged W^X write flip, appeared between those two
samples. The shared stderr trace did not label every JIT byte sample with its
host PID and thread ID, so cross-process interleaving cannot be fully excluded.
The result argues against the runtime instruction rewrite directly zeroing the
page, but does **not** yet establish how it was cleared or why PCRE2 called its
entry. A follow-up trace needs PID/thread and mapping-generation identifiers.
JIT byte and syscall tracing were temporary local instrumentation and were
removed after this test.

### W^X page replacement root cause and fix (2026-11-17)

`page_is_mapjit()` in `src/gmem.c` tested `mach_vm_region().max_protection`
for WRITE+EXECUTE. A native probe showed that an ordinary anonymous RW page
also has maximum RWX protection, including after its current protection is
changed to RX. The check therefore misidentified a W^X-emulated page as a
MAP_JIT reservation. On its next write fault, the MAP_JIT branch replaced the
still-live guest code page with a fresh zero page using `mmap(MAP_FIXED)`.
This explains the valid first exec sample, missing W^X write-flip log, and
later zero instructions. The check now uses **current** protection: the
ordinary W^X page is RW or RX, while the actual MAP_JIT reservation is RWX.

The new `tests/run-wx-rewrite.py` compiles and runs an aarch64 Linux guest
program in a disposable Arch root. Before the fix, its second execution hit
an illegal zero instruction at the retained `ret` word. With the fix, it
returned 7, rewrote the first instruction, then returned 9 with the `ret`
word intact; the W^X counters recorded two exec flips and one write flip.
Both CLI and GUI builds passed. This directly verifies the page preservation
regression, but **not yet** a complete Plasma startup. A bounded GUI boot in
the disposable clone reached getty and authenticated the throwaway user, but
the session lacked `XDG_RUNTIME_DIR`; KWin and Plasmashell did not persist.
The Plasmashell JIT-on acceptance criterion remains to be rerun once that
separate session setup issue is resolved.

# 17 November 2026 logind update

The earlier empty `XDG_RUNTIME_DIR` finding above was reproduced and traced.
The `CreateSession` request and peer credentials were valid; kqueue pidfds could
not cross Darwin `SCM_RIGHTS`, and dbus-broker also required the existing
outgoing-queue compatibility path. An isolated authenticated TTY login now
creates a real logind session, runtime directory, user manager, and user bus
with the `run.sh --init` flags. See `LOGIND-SESSION-PIDFD.md`. This does not yet
establish a visible or persistent Plasma desktop.
