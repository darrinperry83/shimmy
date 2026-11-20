#!/usr/bin/env python3
"""A tmpfs mount of /run/user/<uid> materializes the runtime directory (mode 0700).

On native Linux, systemd-logind mounts /run/user/<uid> as a tmpfs so the display
manager gets XDG_RUNTIME_DIR. The shim used to return EPERM for that exact mount,
leaving /run/user empty and kwin_wayland unable to start. Now it must create the
directory with mode 0700 and succeed.
"""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-runuser-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        (root / "run").mkdir()
        guest = root / "tmp/guest-runuser"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-runuser.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-runuser"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "RUNUSER_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("RUNUSER_OK: tmpfs /run/user/<uid> materialized with mode 0700")


if __name__ == "__main__":
    main()
