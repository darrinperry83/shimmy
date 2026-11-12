# Changelog

All notable changes to shimmy are documented here. Format follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased] — early stage

### Added
- Arch Linux (glibc, aarch64) guest bootstrapped locally; rootfs not stored in git.
- rdmetal Mesa driver (submodule) with PKGBUILD + GitHub Actions CI.
- Stock Hyprland and KDE Plasma desktops rendering on rdmetal.
- mac apps owned by the compositor as Wayland surfaces (window creation + IOSurface content).
- ALSA → CoreAudio audio path (PipeWire/WirePlumber session in progress).
- Scanout injection (direct-to-display present) with SIP/setup instructions.
