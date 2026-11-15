# Shimmy desktop session: implementation plan

Status: design draft, 16 November 2026. This document defines acceptance tests; it does not
claim that the desktop session or full rdmetal renderer already works.

## Product contract

Boot `systemd` in the Arch root, authenticate a Linux user, start an installed Wayland desktop,
lock and unlock it, log out, and return to the greeter. Keep the existing Shimmy host window
throughout. Ctrl+Option+F1–F6 still shows text consoles; F7 returns to the graphical seat.
Hyprland is the first compositor; Plasma must be able to use the same host contract later.

The Mac supplies physical display, keyboard, pointer and GPU services. Linux owns accounts,
PAM authentication, systemd services, logind sessions, the Wayland compositor and its lock
surface. A Linux session lock protects that Linux session; it does not lock the underlying
macOS login session or every other Mac app.

## Current evidence and gaps

| Area | Verified or present | Missing for this contract |
| --- | --- | --- |
| Boot and TTY | systemd reaches getty; isolated tests logged into Bash; tty1–tty6 gettys and TTY2 prompt verified | login → DE → logout cycle in the GUI |
| Packages | Arch root has Hyprland and Plasma binaries; ordinary pacman path exists | a configured and tested greeter and locker for this boot mode |
| GPU | rdmetal has a bounded Metal clear/draw/resource path | its Gallium screen still uses softpipe and a hybrid fallback; full compositor/GLES workload is unproved |
| Display | host window renders VT; standalone desktop launch has a Metal presentation path | systemd-launched compositor presenting into that *same* host window |
| Input | host NSEvents feed the VT and a per-process evdev pump | active compositor process receiving the boot window's input with seat ownership |

Source checkpoints: `src/vt.c`, `src/window.m`, `src/drmasahi.c`, `src/rdgpu_mock_gate.c`,
`src/shim.c`, `run.sh`; the external Mesa source currently used to build rdmetal is
`integrated-mesa-next/mesa/src/gallium/drivers/rdmetal/rdmetal_screen.c` and
`rdmetal_clear_context.c` in the existing development workspace. The direct Omarchy launcher
currently enables `RDMETAL_LEDGER_MOCK` and `RDMETAL_SAME_IMAGE_METAL_FIXTURE`. The DRM flip
path reads a CPU BO and calls `gpuwin_present_crtc` with bytes. These facts make a visible
desktop insufficient proof of the full-GPU requirement.

## Target process and data flow

```
Mac: one persistent shim_gui host process
     ├─ one NSWindow/CAMetalLayer, VT renderer, input capture, presenter
     └─ systemd guest PID 1 on its Linux execution thread
             ├─ getty@tty1..6
             ├─ logind + D-Bus + user manager
             └─ greetd → authenticated user session → Hyprland
                           ├─ Linux Wayland apps
                           ├─ hyprlock (session lock protocol + PAM)
                           └─ rdmetal → host Metal broker → IOSurface
                                          ↓ authenticated surface handoff
                              persistent shim_gui presenter
```

The presenter must be a host service owned by the original GUI process. The first target is
the existing Shimmy window on one selected display. Later multi-monitor support may add
one host-owned presentation window per display, but never one window per guest process.
Forked guest programs must not create independent Mac desktop windows. The active compositor sends a
completed IOSurface and fence to the presenter, which displays it in the existing layer.
The host routes keyboard and pointer input to the active Linux seat; text VTs retain their
own input route. Resize and display changes travel in the reverse direction. Use a bounded
frame queue and release surfaces only after presentation has finished.
While a text VT is selected, do not paint compositor frames over it. Return to the current
graphical frame only on VT7. When a session locks or ends, discard its last unlocked frame.

The per-boot channel needs a random identity, host-kernel peer validation, and explicit
ownership transfer. Guest-reported UID alone is not a host security boundary. Only the
active compositor/seat may submit display frames or receive desktop input. A crashed or
logged-out compositor must lose those rights and return the existing window to the greeter
or TTY. Reject stale frames and input from an earlier session.

## GPU contract

“Full rdmetal” means the compositor's required GLES operations and the representative app
set perform their rendering on the Apple GPU through Metal. A Metal final blit of a CPU
rasterized desktop does not satisfy it. CPU work for ordinary application logic is fine;
software *graphics* fallback must be disabled for the acceptance run.

Work from the actual Gallium operations used by Hyprland, the greeter, hyprlock and the test
apps. Complete shader/state, texture sampling/upload, render targets, vertex and index data,
blending, clipping, clears and synchronization needed by that workload. Replace the mock
broker/session and test-only dma-buf carrier with a production, authorized IOSurface path.
The compositor's KMS framebuffer must resolve to the GPU surface, not a CPU dumb BO copied
into `gpuwin_present_fmt`.

Instrumentation must report: driver/backend name, every softpipe fallback, Metal draw/clear
counts, CPU framebuffer-copy bytes, surface generations, fences, dropped frames and present
latency. `GL_RENDERER` alone is not proof. Run the desktop with fallback disabled and assert
zero fallback calls and zero CPU framebuffer staging during ordinary composition. Test
multiple windows, text, textures, animation, resizing, screen lock and resume. Then measure
frame time, CPU use, memory and power against the current hybrid path.

Performance acceptance is comparative and trace-based: ordinary composition must have no
per-frame CPU framebuffer readback/upload, no unbounded waits on the UI thread, and a bounded
surface pool. Record median and 95th-percentile frame latency at the actual display refresh
rate, CPU/GPU use, memory and power for the same scenes before and after. A faster-looking
demo with higher sustained CPU or dropped frames does not pass simply because Metal appears
in the call stack.

## Linux session contract

Use a standard Arch display manager. First candidate: greetd. Its text greeter can establish
the authentication/session plumbing during development; the product gate includes a graphical
greeter rendered through the same GPU and window path. Authenticate through the root's PAM stack;
no host-side stored password, autologin or Shimmy-specific fake login. Start Hyprland through
its installed session entry as the selected Linux user, with a real `XDG_RUNTIME_DIR`, user
D-Bus and systemd/logind session. Remove the shim's default `WAYLAND_DISPLAY=wayland-0`
from the greeter/compositor environment so it selects its native display backend; clients
receive the compositor's Wayland socket after the session starts. Use the real seat/device
grant and revoke path; treat `LIBSEAT_BACKEND=noop` only as a diagnostic until that is proven.

Install a standard locker such as hyprlock, using its session-lock protocol and Linux PAM.
While locked, Linux apps cannot receive keyboard/pointer input or expose their old surface
through the presenter. Switching to a text TTY is allowed as on Linux, but returning to the
graphical VT must show the locked session. Logout stops the user compositor, revokes its
surface/input channel, and restarts the greeter in the same window. A crash fails closed to
a safe TTY or greeter; it must never reveal the previous unlocked frame.

### Visible state transitions

| State | Visible in the host window | Who receives input | Allowed next state |
| --- | --- | --- | --- |
| Boot | systemd text console | active VT | greeter or recovery TTY |
| Greeter | Linux greeter | greeter seat | authenticating or TTY |
| Authenticating | greeter, with no desktop pixels | greeter/PAM | desktop on success; greeter on failure |
| Desktop | compositor frame | active Linux session | locked, logout, or TTY |
| Locked | session-lock surface only | locker | desktop after PAM success; TTY |
| Logout/crash | cleared frame, then greeter or recovery TTY | no old session | greeter or TTY |

The presenter must own this state, not infer it from the arrival of a frame. A late frame
from a logged-out session cannot reopen or unlock the desktop. A TTY switch changes only
which surface is visible and where keyboard events go; it does not authenticate or terminate
the Linux session.

## Milestones and acceptance gates

1. **Measured baseline.** Capture current renderer/fallback/CPU-copy counters for the existing
   standalone Hyprland path and a systemd boot. Record exactly which operations reach Metal.
   No product claim changes based on screenshots or `GL_RENDERER` alone.
2. **Production GPU path.** A strict, fallback-disabled rdmetal run completes compositor,
   greeter, locker and representative GLES app scenes. The mock fixture is off; GPU and fence
   evidence is recorded. Unsupported operations fail visibly during development.
3. **Single-window bridge.** An isolated systemd boot moves VT → test GPU surface → VT in one
   persistent NSWindow. A second guest process cannot open another desktop window. Input,
   resize, process exit, stale-frame rejection and bounded queue tests pass.
4. **Authenticated session.** Disposable Arch root boots greetd, logs in a throwaway user via
   PAM, starts Hyprland, launches an app, logs out and returns to greetd. A failed password
   does not start a session. No personal credentials enter tests or logs.
5. **Lock and recovery.** Lock/unlock is authenticated; VT round-trip remains locked; compositor
   crash and logout clear old pixels and release device/input ownership. Test both screens and
   window resize/hotplug without changing the live Mac display configuration.
6. **Product readiness.** Repeat the cycle on the installed root with the user's participation
   for password entry, then a cold boot. Compare performance and power to the baseline and
   document known unsupported apps/operations. Plasma reuses the presenter and session
   contract rather than receiving a separate host integration. Update the README and
   `docs/HOW_IT_WORKS.md` to describe only paths proven by these gates.

Use disposable copy-on-write roots for service, package and credential tests. Do not change
the live root or restart the user's current GUI merely to run a test. Keep the current TTY
boot path as a recovery route throughout development.

The rdmetal Mesa source used for the installed driver currently lives outside this Git
checkout; `install.sh` expects a `rdmetal/rdmetal-libs` artifact directory that this clone
does not track. Before a product build, pin the exact driver source and build inputs in a
reproducible dependency (submodule, separate versioned package, or equivalent), record the
installed binary hash, and test that Shimmy and the driver agree on the broker protocol.
Otherwise a passing Shimmy commit can still ship a different graphics driver.

## Decisions made and open technical proofs

The host window stays in the original process; Linux owns the greeter/session/lock lifecycle.
Hyprland is first, but the transport is compositor-independent. Full GPU rendering and
same-window presentation are separate gates; both are required before calling the desktop
complete. The open proofs are the Gallium operation coverage, production surface/fence
transport, logind seat/device behavior, and input ownership across child processes. Choose
the final greeter UI only after those foundations pass; it must use the same session path.
