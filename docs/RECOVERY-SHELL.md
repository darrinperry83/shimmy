# Recovery shell

The **recovery** rootfs profile is a minimal, independent rescue image: it boots straight into a
full-screen **graphical installer** on the Linux side (rendered through the shim's Wayland/DRM path),
and from there you can connect WiFi, reinstall a fresh rootfs, drop to a shell with the disassembly
tools, inspect bridged Bluetooth/USB devices, or reboot back into macOS.

```sh
./install.sh --rootfs recovery     # build the recovery rootfs + installer
./run.sh                           # boot straight into the graphical installer
# or, directly:
./shim_cli rootfs /usr/local/bin/shimmy-installer-start
```

## What ships in `recovery`

The recovery rootfs is a stock **Arch Linux ARM aarch64** base plus a minimal, purpose-built package
set (no desktop, no window manager, no GPU driver):

| Purpose | Packages |
|---|---|
| shell + core | `busybox` `coreutils` |
| WiFi | `wpa_supplicant` `dhcpcd` |
| disassembly / debugging | `binutils` (objdump) `gdb` `python` |
| graphical installer | `qt6-base` `qt6-wayland` `pyside6` (Qt6 Widgets via PySide6) |
| installer launch | `cage` (minimal kiosk Wayland compositor) |
| Bluetooth bridge | `bluez` `bluez-utils` |
| USB bridge | `usbutils` |

The install step also drops two scripts into `/usr/local/bin`:

- **`shimmy`** — the CLI; `shimmy recover` is the scriptable backend (see below).
- **`shimmy-installer`** — the graphical installer app.

## The graphical installer

**Toolkit:** Qt6 (`qt6-base`) written in Python against **PySide6**, rendered on the shim's Wayland
display via `qt6-wayland`. It is a single full-screen QtWidgets window with these screens:

1. **Connect to WiFi** — a network picker: `wpa_supplicant` scans, the SSID list is shown, you type
   the passphrase, then `dhcpcd` brings the interface up.
2. **Install a rootfs** — choose `arch` or `omarchy`, then a progress bar while the tarball downloads
   and extracts over the target (`/` by default).
3. **Open a shell** — an in-app terminal running `/bin/bash`, with `gdb`, `objdump` and `python` on
   `PATH`.
4. **Reboot into macOS** — a clean guest shutdown that ends the shim's guest and triggers the
   warm-switch back to macOS.
5. **Bluetooth & USB devices** — lists the bridged devices (see below).

**Launch path:** recovery boots to a root autologin on `tty1`, which runs
`/usr/local/bin/shimmy-installer-start`, i.e.

```sh
dbus-run-session -- cage -s -- /usr/local/bin/shimmy-installer
```

`cage` is a minimal kiosk Wayland compositor that runs the installer as its single full-screen
client on the shim's DRM node — the same DRM-master path Hyprland/KDE already use. A
`shimmy-installer.service` unit (enabled for `multi-user.target`) is installed as the equivalent
systemd launch path.

## `shimmy recover` — the scriptable backend

`shimmy recover` is the command the installer buttons call, and it is also usable directly from the
recovery shell or from the host for testing. Every action has a `--dry-run` that prints the exact
steps without acting:

```sh
shimmy recover --dry-run                       # full plan: WiFi + fresh-rootfs install
shimmy recover --scan                          # list SSIDs (wpa_cli scan_results)
shimmy recover --ssid MyNet --psk secret       # connect (wpa_supplicant + dhcpcd)
shimmy recover --profile omarchy               # download + extract a fresh rootfs
shimmy recover --profile arch --url <URL> --target /mnt
shimmy recover --devices                       # bluetoothctl list + lsusb
shimmy recover --shell                         # exec /bin/bash (gdb/objdump/python)
shimmy recover --reboot                        # warm-switch back to macOS
shimmy recover                                 # launch the graphical installer
```

The fresh-rootfs download uses the Arch Linux ARM base tarball for `arch`
(`http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz`) and a shimmy release asset for
`omarchy` (`…/rootfs-omarchy-aarch64.tar.gz`, overridable with `--url` or `SHIMMY_RELEASE_URL`).

## Hardware bridges (shim-side, like audio → CoreAudio, input → HID)

The recovery rootfs carries the guest-side packages; the shim supplies the bridge on the macOS side:

| Device | Guest side | Shim bridge | Result |
|---|---|---|---|
| Bluetooth | `bluez` + `bluez-utils` | host **CoreBluetooth** | Linux sees an **HCI device** (`bluetoothctl`) |
| USB | `usbutils` | host **IOKit** via `/dev/bus/usb` | `lsusb` lists the host's USB devices |

These are the same pattern as the existing audio (PipeWire/ALSA → CoreAudio) and input
(evdev/libinput → HID) bridges: the guest runs the stock Linux userspace, the shim translates the
device surface onto a real macOS driver. The bridge code itself lives in the shim (not in the
recovery rootfs); the recovery profile only guarantees the guest-side packages are present so the
bridge has a working Linux counterpart.

## Booting into recovery

1. `./install.sh --rootfs recovery` — builds the shim, bootstraps the Arch base, installs the
   recovery package set, and drops in the installer + launch path.
2. `./run.sh` — the shim boots the recovery rootfs; cage launches the full-screen installer.

> The recovery rootfs is a local build artifact (`rootfs/`, gitignored), the same as `arch` and
> `omarchy`. It never takes over the host: it runs inside the shim's Linux environment, and
> "Reboot into macOS" returns to macOS via the shim's warm-switch (the shim exits cleanly and the
> real WindowServer relaunches).
