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

## On-screen frame bridge trace (2026-11-17)

An isolated systemd GUI boot reached the throwaway user's authenticated Bash
session with `XDG_RUNTIME_DIR=/run/user/22001`; `startplasma-wayland` started
KWin and Plasmashell. The host window remained black. With
`RDGPU_PRESENT_STATS=1`, the persistent GUI owner (PID 19614) accepted two
3600x2338 CRTC 2 frames from the KWin host process (PID 20015). Both presenter
callbacks returned 0. Direct inspection of the shared frame's 8,416,800 RGB
pixels found all zero, so the bridge delivered black content rather than
dropped those two frames. A temporary colored frame through the same receiver
was logged as nonblack with present status 0, then restored. This does not
establish a visible desktop or GPU rendering.

The disposable root then exhausted host disk space. systemd reported `No space
left on device` while restarting logind and udev, so later service behavior in
that boot is not a valid Plasma stability result. The test boot and disposable
root were stopped and removed. Further compositor work needs sufficient disk
headroom and a bounded guest journal before another on-screen run.

## Authenticated login to Plasma handoff (2026-11-17)

The GUI boot showed `localhost login:` on tty1. The shim had been losing the
guest's `argv[0]` across exec, so `/bin/login`'s `-bash` became `/bin/bash` and
the user's login profile was skipped. Commit `c9e4d92` preserves guest argv0;
the focused regression now reports `login_shell on` and `ARGV0=-bash`. Both
CLI and GUI builds passed.

With a throwaway account and a profile in a disposable root, an on-screen
password login ran that profile, created a logind user session and
`XDG_RUNTIME_DIR`, and launched `startplasma-wayland`. The guest journal logged
`Started KDE Plasma Workspace`; KWin and Plasmashell remained running during
the observation. The host window was still black. The cross-process frame file
advanced to sequence 4, but its sampled pixels were all zero and no later
frame appeared. This is **not** a visible desktop or a validated logout cycle.
Terminating the test's `startplasma-wayland` process logged `Session c1 logged
out. Waiting for processes to exit`, while KWin remained running. The entire
disposable boot was then stopped.

This run generated large host core dumps and filled the disk; only dumps
created during the test were removed and free space recovered. The throwaway
password/profile, test unit masks and input FIFO were restored or removed.
Future GUI boot probes must disable core dumps in the parent process before
launch. The next presentation investigation is why KWin emits only an initial
black frame after authenticated Plasma starts; a live process alone does not
prove continued rendering.

## Black-frame stall thread sample (2026-11-17)

A second on-screen authenticated `run.sh --init` boot in a disposable COW root
again delivered exactly two 3600x2338 all-black frames to the owner, with
`present=0`, then no further frames. The frame carrier reached sequence 4.
The owner-side bridge did not drop either frame.

A three-second native stack sample of the KWin host process showed its guest
main thread in Shimmy's Linux `read` translation, while the `eDP-1` output
thread was waiting in a guest futex. LLDB registers identified the blocked call
as `read(67, ..., 152)`. Host `lsof` identified fd 67 as a blocking pipe whose
write end, fd 68, was also open in KWin; `fcntl(67, F_GETFL)` returned 0.
That fd is not the DRM event socket. KWin also had a defunct child. These
observations do not yet establish which KWin subsystem owns the pipe or why
its child exited. No DRM flip-event, syncobj, or BO-mapping change is justified
by this trace alone. The next investigation should trace creation and ownership
of fd 67/68 and the child's exit, then establish why the read has no writer.

The first trace run generated large host core files despite a parent shell
`ulimit -c 0`; systemd services can set their own limits. A subsequent
clone-only `DefaultLimitCORE=0` in system and user manager configuration plus
`LimitCORE=0` for KWin produced no core files during the bounded observation.
The test processes and their core files were removed; the live root and
session were not changed. No visible desktop or logout cycle was proven.

## Xwayland child failure behind KWin's pipe wait (2026-11-17)

A later bounded pipe/fork trace identified fd 67 as KWin's Xwayland readiness
pipe, not its DRM event fd. The child wrote a 512-byte Qt process-start error
to a separate pipe, then exited before `execve(Xwayland)`. The error was
`EBADF: failed to get file descriptor flags`; the child had called
`fcntl(-1, F_GETFD)`. KWin retained the readiness pipe's write end, so its
blocking `read(67, ..., 152)` could not receive data or EOF. This identifies
the immediate wait, but does not yet prove the cause of the black frame data.

KWin 6.8.2 source identifies the invalid descriptor's origin: `EisBackend`
opens its Xwayland EIS listening socket with Linux `O_PATH|O_CLOEXEC`, stores
the result in `FileDescriptor`, and passes it to `XwaylandLauncher` without
checking validity. That launcher calls `fcntl(F_GETFD)` on each passed fd in
the child. The Shimmy syscall trace recorded the preceding guest open of
`/run/user/22001/kwin-xwayland-eis-socket.<pid>` returning host errno 102
(`Operation not supported on socket`) from the namespaced host socket path;
the resulting descriptor is `-1`. Shimmy currently translates `O_PATH` to a
normal Darwin `O_RDONLY` open, which does not provide Linux `O_PATH` semantics
for this listening socket. This identified the startup failure, not a proven
pipe/fork fd-inheritance bug.

## Xwayland handoff after the O_PATH bridge (2026-11-18)

Commit `f3424fd` added a socket-inode `O_PATH` reference and its
`/proc/self/fd/N` handoff. A fresh disposable Arch boot after the additional
runtime-directory ownership fix `b79a07d` authenticated a throwaway user and
started KWin, Plasmashell, Xwayland and a foot Wayland client. In a bounded
45-second GUI observation KWin and foot kept the same PIDs. The cross-process
frame carrier reached sequence 34 (17 frames) and contained nonblack RGB
pixels, versus the old sequence 4 (two all-black frames). A second fresh-run
regression check reached sequence 26 with nonblack RGB and Xwayland running.
`tests/run-interactive-login.py` has an opt-in `RD_TEST_FRAME_PROGRESS=1`
assertion for more than two frames, nonblack RGB and a running Xwayland process.

A repeat that reused the prior boot's `/run` produced only two black frames:
foot chose a stale `wayland-1.lock`, then failed to connect to the old socket.
Resetting `/run` in the disposable clone restored the working result. This
failure was a test-fixture lifecycle error, not proof that fd inheritance
regressed. The installed root and live session were not changed.

The desktop is still slow. The frame carrier moves 3600x2338 BGRA images
(33,667,200 bytes each) through CPU memory, and the installed rdmetal Mesa
screen still falls back to softpipe. In one bounded run foot waited roughly
6.7 seconds between Wayland frame callbacks; Xwayland and Plasmashell each
used substantial CPU while KWin stayed alive. This clears the pipe-stall
gate, not the full-GPU or usable-performance gates.

The supplied isolated RD Metal driver (`8e5e02ccecb425a2c41740e0a86cec2920d3fcac62677ab40d017d97a3b8380c`)
was installed only in another disposable Arch clone. After resetting that
clone's `/run`, the same 45-second GUI probe authenticated, kept KWin,
Plasmashell and foot alive, and reached frame sequence 34 with nonblack RGB.
The first attempt with a reused `/run` stalled at the password prompt, so it
was not a graphics result. This validates the fixed driver against this
session's startup and frame path. It does not establish a frame-rate gain:
the probe observed only 17 frames and did not disable the hybrid softpipe
fallback or prove Metal execution for those frames. The live installed driver
was not replaced.

## Native Metal frame progress in the systemd Plasma boot (2026-11-18)

An isolated on-screen `shim_gui /sbin/init --system` boot authenticated a
throwaway user on tty1 and started `startplasma-wayland`, KWin, Plasmashell,
and foot. This was the full init/login path, not a standalone compositor run.
The local Metal broker and host display window exchanged the same IOSurface
through the authenticated DRM-device handoff after logind acquired DRM master.
The associated Shimmy change is `a4858f9`; the RD Metal Gallium driver change
is `5913b55`.

The first Metal draw sampled an all-black 273x273 source, and both its target
and the host window were black. A subsequent valid indexed triangle draw used
Gallium `start=48` and `index_bias=1`; the driver had rejected both parameters
and permanently failed its context. Supporting bounded index offsets and bias
made the 3600x2338 target nonblack. The next rejected draw was a four-vertex
`MESA_PRIM_TRIANGLE_FAN`; bounded generic fan conversion let rendering
continue. These are Gallium draw semantics, with no KWin-specific dispatch.

After a clean rebuild and no RD Metal or Mesa draw flags, the isolated boot
again reached login and Plasma. Four host-window samples contained 0, 0,
7,440, and 8,148 nonblack pixels out of 67,080 sampled, respectively. A later
run with the bounded resource-hold cleanup yielded 0, 7,440, 8,148, and 8,888.
The host presenter therefore receives real nonblack Metal content in this
path. CLI and GUI builds and the native DRM-master authorization regression
passed; the latter requires permission to register a temporary Mach bootstrap
name. A native RD Metal resource-lifetime test also passed. The installed live
Arch root and its driver were not modified.

This is an initial visible GPU path, not full native-equivalent GPU support.
The direct Gallium implementation still has limited primitive, format, and
render-state coverage, retains a hybrid softpipe fallback, and compiles MSL
and pipeline state per draw. Sustained frame rate, zero-fallback composition,
greeter/lock/logout, takeover, and both-display operation remain unverified.

### Gallium viewport orientation (2026-11-18)

The first visible Metal-rendered foot terminal was vertically inverted. The
RD Metal draw path saved Gallium's viewport but passed clip-space Y directly
to Metal. Driver commit `825a63d` applies Gallium's scale and translation to
framebuffer pixels, then maps those pixels to Metal's top-left target. This is
the general viewport contract, not a KWin-specific correction.

After rebuilding the driver and copying it into a fresh disposable Arch root,
the same on-screen init/login/Plasma/foot test showed the terminal title and
text upright. Host-window samples were 0, 0, 7,360, and 8,232 nonblack pixels
out of 67,080 across the first four frames. The direct-resource bind test and
driver build passed. This proves the observed orientation correction for that
path; broader viewport, scissor, multisample, and performance coverage remains
to be tested. The installed live Arch root was not modified.
