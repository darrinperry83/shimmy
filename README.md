# RMatrixOS-shim

The macOS-side runtime for RMatrixOS: syscall translation, the Wayland server
(`wlsrv.c`), input, the Metal present path, and the mac-window→Wayland-surface bridge.

The Linux guest runs inside this shim; the rdmetal Mesa driver (see `RMatrixOS-rdmetal`)
renders the compositor, and the scanout (see `RMatrixOS-scanout`) presents directly to
the display.

## Build
```sh
./build.sh gui    # shim_gui (the display server host)
./build.sh cli    # shim_cli  (headless guest runner)
```
