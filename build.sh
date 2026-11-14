#!/bin/bash
# Build the Linux-on-macOS shim.
#   ./build.sh cli    -> ./shim_cli  (headless: no Metal/Cocoa; busybox & friends)
#   ./build.sh gui    -> ./shim_gui  (Metal window + in-process X11 server)
#   ./build.sh        -> both
set -e
cd "$(dirname "$0")"
SRC=src
SDK=$(xcrun --show-sdk-path)
mkdir -p build

# MIG-generated Mach exception server (regenerate once; it is a build artifact, not a source).
if [ ! -f build/mach_excServer.c ]; then
  ( cd build && mig -arch arm64 "$SDK/usr/include/mach/mach_exc.defs" >/dev/null )
fi

CFLAGS="-arch arm64 -O2 -Wall -Wno-unused-parameter -I$SRC"
# Record WHAT SOURCE, and WHICH BINARY, each build produced -- so a gate can refuse to measure
# something it did not build. tools/assert-fresh.sh reads these.
#
# MTIME CANNOT DO THIS JOB, and that is worth stating because the mtime version was written first
# and PASSED the exact artefact it was built to catch: `git checkout` sets the restored file's
# mtime to NOW, so a binary from an old commit looks brand new. Content is the only honest signal.
#
# BOTH halves are recorded. A source-only stamp also reports "ok" after `git checkout shim_wlh`
# swaps the binary and leaves the stamp behind -- again caught by negative control, not by review.
srcsha() { cat "$SRC"/*.c "$SRC"/*.h "$SRC"/*.m third_party/*.m third_party/*.mm third_party/*.h third_party/spirv-cross/*.h third_party/spirv-cross/*.hpp build.sh 2>/dev/null | shasum -a256 | cut -d' ' -f1; }
stamp()  { printf '%s %s\n' "$(srcsha)" "$(shasum -a256 "$1" | cut -d' ' -f1)" > "build/$1.srcsha"; }

CORE="gmem.c guest_range_ledger.c elfload.c syscalls.c syscalls_ext.c signalfd_shim.c timerfd_shim.c bootstrap.c net.c vdso.c fastcall.c hostcall_scoped.c drmasahi.c rdgpu_surface_handoff.c v4l2cam.c present_feedback.c snd_alsa.c coreaudio_host.c"
RDGPU="rdgpu_guest_syscall.c rdgpu_mock_gate.c rdgpu_virtual_carrier.c"

build_third_party() {
  # Vendored rdgpu host objects (see third_party/README.md). These used to be
  # linked as pre-built .o files from sibling trees OUTSIDE the repo
  # (../integrated-mesa-next/..., ../async-draw-next/..., ../broker-draw-next/...);
  # they are now built from source inside the repo. rdmetal_resource.m and
  # rdgpu_broker.m use manual reference counting (no ARC); rdgpu_draw_host.mm is
  # Objective-C++ and needs ARC + C++17 to compile spirv_cross::CompilerMSL.
  echo "== third_party =="
  rm -rf build/third_party && mkdir -p build/third_party
  ( cd build/third_party && clang $CFLAGS -ObjC -I../../third_party -c ../../third_party/rdmetal_resource.m -o rdmetal_resource.o )
  ( cd build/third_party && clang $CFLAGS -ObjC -I../../third_party -c ../../third_party/rdgpu_broker.m -o rdgpu_broker.o )
  ( cd build/third_party && clang $CFLAGS -fobjc-arc -std=c++17 -I../../third_party -I../../third_party/spirv-cross -c ../../third_party/rdgpu_draw_host.mm -o rdgpu_draw_host.o )
}

build_cli() {
  echo "== shim_cli =="
  rm -rf build/cli && mkdir -p build/cli
  ( cd build/cli && clang $CFLAGS -DNO_GPU -c ../../$SRC/shim.c -o shim.o )
  for f in $CORE $RDGPU; do
    ( cd build/cli && clang $CFLAGS -DRDGPU_CLIENT -c ../../$SRC/$f -o "${f%.c}.o" )
  done
  clang -arch arm64 -o shim_cli build/cli/*.o build/third_party/rdgpu_broker.o build/third_party/rdmetal_resource.o build/third_party/rdgpu_draw_host.o build/mach_excServer.c \
    third_party/lib/libspirv-cross-msl.a \
    third_party/lib/libspirv-cross-glsl.a \
    third_party/lib/libspirv-cross-core.a \
    -framework Metal -framework IOSurface -framework Foundation \
    -framework Security -framework CoreFoundation -framework AudioToolbox \
    -framework AudioUnit -framework CoreAudio -lc++
  codesign --force --sign - --entitlements $SRC/ent.plist shim_cli
  stamp shim_cli
  echo "   -> ./shim_cli"
}

build_gui() {
  echo "== shim_gui =="
  rm -rf build/gui && mkdir -p build/gui
  ( cd build/gui && clang $CFLAGS -c ../../$SRC/shim.c -o shim.o )
  for f in $CORE x11srv.c glxdecode.c wlsrv.c; do
    ( cd build/gui && clang $CFLAGS -c ../../$SRC/$f -o "${f%.c}.o" )
  done
  # The rdmetal GPU broker (syscall 0x2002) — same guest-side syscall the CLI ships, so the
  # GUI desktop can create Metal-backed (broker) resources instead of falling back to softpipe.
  for f in $RDGPU; do
    ( cd build/gui && clang $CFLAGS -DRDGPU_CLIENT -c ../../$SRC/$f -o "${f%.c}.o" )
  done
  for f in gpu_bridge.m window.m named_clipboard.m secondary_window.m x11srv_render.m glbridge.m capresent.m rdgpu_scanout_client.m mac_surface_host.m; do
    ( cd build/gui && clang $CFLAGS -ObjC -fobjc-arc -c ../../$SRC/$f -o "${f%.m}.o" )
  done
  clang -arch arm64 -o shim_gui build/gui/*.o build/third_party/rdgpu_broker.o build/third_party/rdmetal_resource.o build/third_party/rdgpu_draw_host.o build/mach_excServer.c \
    third_party/lib/libspirv-cross-msl.a \
    third_party/lib/libspirv-cross-glsl.a \
    third_party/lib/libspirv-cross-core.a \
    -framework Metal -framework MetalKit -framework Cocoa -framework QuartzCore -framework CoreGraphics -framework Accelerate -framework IOSurface -framework IOKit -framework CoreServices \
    -framework AudioToolbox -framework AudioUnit -framework CoreAudio -framework Security -lc++
  codesign --force --sign - --entitlements $SRC/ent.plist shim_gui
  stamp shim_gui
  echo "   -> ./shim_gui"
}


build_wlh() {
  # Diagnostic: CLI core (no Metal/Cocoa, no window-server lock) + the Wayland compositor.
  echo "== shim_wlh =="
  rm -rf build/wlh && mkdir -p build/wlh
  ( cd build/wlh && clang $CFLAGS -DNO_GPU -DWL_HEADLESS -c ../../$SRC/shim.c -o shim.o )
  for f in $CORE wlsrv.c wlhead_stub.c; do
    ( cd build/wlh && clang $CFLAGS -DNO_GPU -DWL_HEADLESS -c ../../$SRC/$f -o "${f%.c}.o" )
  done
  # capresent.m in the HEADLESS build. S1's REJECT reason 4 and S3's own §2 both said "the CA arm
  # can never be gated headlessly" -- and S3 disproved it against themselves: it was a BUILD CHOICE,
  # not a property of CALayer/IOSurface. A CALayer can be built and IOSurfaces published with no
  # window and no GUI lock. Linking it here is what lets c1-gate cover the CA pixel path headlessly,
  # which is the difference between shipping an arm nobody can gate and one anybody can.
  ( cd build/wlh && clang $CFLAGS -DNO_GPU -DWL_HEADLESS -ObjC -fobjc-arc -c ../../$SRC/capresent.m -o capresent.o )
  clang -arch arm64 -o shim_wlh build/wlh/*.o build/mach_excServer.c -framework Accelerate \
    -framework IOSurface -framework QuartzCore -framework CoreGraphics -framework Foundation \
    -framework AudioToolbox -framework AudioUnit -framework CoreAudio
  codesign --force --sign - --entitlements $SRC/ent.plist shim_wlh
  stamp shim_wlh
  echo "   -> ./shim_wlh"
}

build_capres() {
  # The CALayer/IOSurface path's own correctness test. No window, no GUI lock, no guest: it
  # checks the bytes handed to WindowServer, which is the half that can be checked without the
  # screen. See src/test_capresent.m for the three negative controls.
  echo "== test_capresent =="
  clang $CFLAGS -ObjC -fobjc-arc -o test_capresent \
    $SRC/test_capresent.m $SRC/capresent.m \
    -framework Cocoa -framework QuartzCore -framework IOSurface
  echo "   -> ./test_capresent"
}

build_handoff() {
  # Two-process proof of the cross-process dma-buf->IOSurface mach-port handoff.
  # See src/test_handoff.c for the publish/resolve usage contract.
  echo "== test_handoff =="
  clang $CFLAGS -o test_handoff \
    $SRC/test_handoff.c $SRC/rdgpu_surface_handoff.c \
    -framework IOSurface -framework CoreFoundation
  echo "   -> ./test_handoff"
}

build_mac_app() {
  # The tiny "real mac app" (Metal -> shared IOSurface, published via the handoff)
  # plus its standalone validation probe. Both are host-side; shim_gui launches
  # rd_mac_app when RDNATIVE_MAC_APP_TEST=<path> is set.
  echo "== rd_mac_app =="
  clang $CFLAGS -ObjC -fobjc-arc -o rd_mac_app \
    $SRC/rd_mac_app.m $SRC/rdgpu_surface_handoff.c \
    -framework Cocoa -framework Metal -framework QuartzCore \
    -framework IOSurface -framework CoreFoundation
  echo "   -> ./rd_mac_app"
  echo "== rd_mac_probe =="
  clang $CFLAGS -o rd_mac_probe \
    $SRC/rd_mac_probe.c $SRC/rdgpu_surface_handoff.c \
    -framework IOSurface -framework CoreFoundation
  echo "   -> ./rd_mac_probe"
}

build_chooser() {
  # The boot chooser: a tiny, dependency-free root LaunchDaemon binary that runs BEFORE
  # WindowServer and branches macOS vs shimmy. Only libsystem (servers/bootstrap + mach).
  echo "== chooser =="
  clang $CFLAGS -o chooser $SRC/chooser.c
  codesign --force --sign - chooser
  stamp chooser
  echo "   -> ./chooser"
}

case "${1:-both}" in
  cli) build_third_party; build_cli ;;
  capres) build_capres ;;
  wlh) build_wlh ;;
  gui) build_third_party; build_gui; build_mac_app ;;
  chooser) build_chooser ;;
  handoff) build_handoff ;;
  macapp) build_mac_app ;;
  # The default MUST build shim_wlh too. It used to build only cli+gui, and that silently cost a
  # swarm most of an afternoon: an x18 fix landed, `./build.sh` was run, shim_cli and shim_gui were
  # rebuilt, and `./shim_wlh` was left 24 MINUTES OLDER THAN THE COMMIT BEING TESTED. The stale
  # binary then reported 37 images stranded on the racy x18 path plus a hard fault in libharfbuzz,
  # all of which had already been fixed -- and it looked exactly like a real regression, because
  # the freshly built shim_cli gate passed at the same time.
  #
  # shim_wlh is the headless-Wayland binary, i.e. the one every GUI measurement that does not want
  # the window-server lock is supposed to use. Making it opt-in put the project's most-used test
  # binary on the one path a routine rebuild skipped. Build time is not the scarce resource here.
  *)   build_third_party; build_cli; build_gui; build_wlh; build_capres; build_mac_app; build_chooser ;;
esac
echo "done. try:  ./shim_cli rootfs /bin/busybox echo hello-from-linux"
