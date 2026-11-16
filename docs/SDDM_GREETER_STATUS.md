# SDDM greeter probe (16 November 2026)

This is an isolated service probe, not a visible greeter or desktop-session result.
The installed Arch root already has `hyprland.desktop` and `plasma.desktop`
Wayland session entries and their commands. It does not have the SDDM package.

In a disposable copy-on-write Arch root, the cached official
`sddm-0.21.0-7-aarch64.pkg.tar.xz` and detached signature were checked with
`pacman-key --verify`: Good signature from Arch Linux ARM Build System at full
trust. Only that clone used `SigLevel = Required TrustedOnly DatabaseOptional`
and `LocalFileSigLevel = Required TrustedOnly`. `pacman -U` placed the SDDM
binary, service and `sddm` account in the clone, although some post-transaction
commands reported failure; do not treat this as a fully clean package operation.
The test enabled `display-manager.service` and `graphical.target` in the clone.

An isolated systemd boot reached `Started Simple Desktop Display Manager` and
`Graphical Interface`. SDDM logged `Initializing`, `Starting`, and `Logind
interface found`, but did not spawn Xorg, KWin, or a visible greeter during the
observation window. The installed SDDM default is `DisplayServer=x11`; applying
the separate `tests/fixtures/sddm-wayland.conf` test configuration did not yet
advance it. The public guest query
`loginctl show-seat seat0 -p CanTTY -p CanGraphical` returned:

```
CanTTY=yes
CanGraphical=no
```

Thus a logind seat exists, but it is not advertised as graphical. The virtual
DRM/udev seat representation is under separate investigation. Re-test SDDM
after `CanGraphical=yes` is backed by a real guest DRM device. SDDM service
startup alone does not prove a greeter, PAM login, Plasma, Hyprland, or GPU
presentation. No live root, Mac display, or existing session was changed.
