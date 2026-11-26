# First visible Linux console experiment

This is an opt-in DEVELOPMENT/VMAPPLE overlay on the working Arch-loader ELF
experiment. It uses XNU's existing video-console renderer and the framebuffer
registered by the platform/display driver, without WindowServer.

Build and link independently of the known-good Arch-loader artifacts:

```
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-console
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-console
```

The builder restores every temporary XNU source edit. The collection retains the
working control's 218 entry identities and release-driver UUIDs. This retains the
existing VM-only no-BTI diagnostic workaround; it is not a hardware boot recipe.

Enable `rdxnu_linux_console=1` alongside the existing ELF/probe arguments in the
disposable VM. At the first Linux ELF stack setup, the overlay enables XNU's text
console and writes a banner. Successful Linux `write` and `writev` calls to file
descriptors 1 and 2 also render there. Original descriptor writes and their return
values are preserved, so the guest job still exports independently checkable logs.
Only stdout/stderr are mirrored; pipes used by the M1 regression remain untouched.

This is **output only**, not a `/dev/tty`, VT, PTY, or interactive Linux shell.
The mirror is bounded to 64 KiB per syscall, uses checked user copies, and may
observe changed user buffers after the original write. It is a diagnostic bridge,
not a final atomic tty write implementation. Display ownership must be exclusive:
use the existing no-WindowServer VM boot profile. Pixel mapping alone does not
prove the virtual GPU is scanning out that framebuffer; verify the guest screen.

Next milestones: verify guest pixels; expose a console descriptor with Linux
`read`/`write` behavior; attach Apple HID keyboard input; implement canonical input,
echo and termios; then execute a real Arch shell through the Linux ABI. Dynamic
Arch shell execution also requires the unfinished loader/memory/syscall subset.

## Observed VM result (2026-11-26)

The kernel overlay boots as UUID `7BB526DA-C8F2-3E30-A7F0-F5EAB18461D4`.
P0, M1 static ELF and the original Arch loader `--help` all pass. The legacy
video console reports 640×1136×32; the actual AppleParavirtDisplay is 2048×1536.
The guest screen stayed on its saved preboot image. `IOFramebuffer` matching
returns zero services, so the legacy console is not a verified scanout route.

`iomfb_probe.c` opens AppleParavirtDisplay directly through the chooser's
IOMobileFramebuffer API: open succeeds; GetMainDisplay returns 0xe00002c0;
GetLayerDefaultSurface returns 0xe00002bc. `display_output.c` tests a new IOSurface
and SwapBegin/SwapSetLayer/SwapEnd instead of depending on a default surface.
It executes the specified Linux child and renders the child's actual pipe output.
Its pixel presentation must be checked separately from syscall success.

The two submodules have been initialized at their pinned revisions. The current
scanout implementation injects into WindowServer; this experiment uses a direct
display connection instead. No rdmetal rebuild is needed for CPU bitmap text.

## Direct presentation passed

The VM screen was visually verified showing the original Arch loader's `--help`
text at 2048×1536. Child exit status was zero, as were SwapBegin, SwapSetLayer and
SwapEnd. See `out/vmapple/console-runtime-result.json` and
`out/vmapple/results/console-display-proof.log`. This is a native Darwin display
host for real Linux ELF output, not an interactive tty or a Linux-kernel boot.

Build the verified display host on macOS:

```
xcrun clang -Wall -Wextra -Werror experiments/xnu-linux-abi/console/display_output.c -framework IOKit -framework CoreFoundation -framework IOSurface -framework CoreGraphics -o experiments/xnu-linux-abi/out/display-output
```

Inside the disposable VM, run as root (the program rejects non-VMAPPLE systems):

```
/var/tmp/rdxnu-display-output /Library/RDXNU-P0/arch-ld.so --help
```

The output proof captures up to 128 KiB, renders after child exit, and retains its
display connection for 60 seconds. Streaming updates, scrollback, input and tty
semantics are future chunks. The experimental shared-command polling job is a
VM development aid only; observed host-file changes were not reliably refreshed
until a VM restart. Do not make this the production service-control mechanism.
