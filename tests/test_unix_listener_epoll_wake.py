#!/usr/bin/env python3
"""Bounded native-vs-Shimmy AF_UNIX listener wake test; no guest or SDDM."""
import pathlib
import resource
import subprocess
import tempfile

repo = pathlib.Path(__file__).resolve().parent.parent


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


with tempfile.TemporaryDirectory(prefix="shimmy-unix-wake-build-") as tmp:
    binary = pathlib.Path(tmp) / "unix-wake"
    subprocess.run(
        ["clang", "-arch", "arm64", "-O2", "-Wall", "-Wextra", "-I", str(repo / "src"),
         str(repo / "tests/test_unix_listener_epoll_wake.c"), str(repo / "src/net.c"),
         str(repo / "src/netlink_route_transport.c"), str(repo / "src/netlink_route_snapshot.c"),
         str(repo / "src/guest_cred_registry.c"), str(repo / "src/pidfd_shim.c"),
         str(repo / "src/snd_alsa.c"), str(repo / "src/coreaudio_host.c"),
         "-framework", "AudioUnit", "-framework", "CoreAudio", "-framework", "AudioToolbox",
         "-o", str(binary)],
        check=True, preexec_fn=no_core,
    )
    for mode in ("native", "shim"):
        for timing in ("queued", "delayed"):
            result = subprocess.run([str(binary), mode, timing], timeout=4,
                                    capture_output=True, text=True, preexec_fn=no_core)
            print(result.stdout + result.stderr, end="")
            if result.returncode:
                raise SystemExit(f"{mode} {timing} exited {result.returncode}")
