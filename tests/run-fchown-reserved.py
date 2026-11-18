#!/usr/bin/env python3
"""Probe fchown fd safety in two modes using only a disposable rootfs."""
from pathlib import Path
import os
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-fchown-reserved-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        (root / "var/lib").mkdir(parents=True)
        file = root / "tmp/fchown-file"
        file.write_bytes(b"fixture")
        original = file.stat()
        for mode in ("virtual", "host"):
            guest = root / f"tmp/guest-fchown-{mode}"
            args = ["clang", "--target=aarch64-linux-gnu", "-nostdlib",
                    "-static-pie", "-fuse-ld=lld", "-Wl,-e,_start"]
            if mode == "host":
                args.append("-DHOST_ONLY")
            args += [str(repo / "tests/guest-fchown-reserved.c"),
                     "-o", str(guest)]
            subprocess.run(args, check=True, timeout=30)
            env = os.environ.copy()
            env["RDUID0"] = "1" if mode == "virtual" else "0"
            result = subprocess.run([
                str((repo / "shim_cli").resolve(strict=True)), str(root),
                f"/tmp/guest-fchown-{mode}"], env=env, capture_output=True,
                text=True, timeout=30, preexec_fn=no_core)
            if result.returncode or "GUEST_FCHOWN_RESERVED_OK" not in result.stdout:
                raise RuntimeError(f"{mode} guest failed rc={result.returncode}\n"
                                   f"{result.stdout}\n{result.stderr[-2000:]}")
        after = file.stat()
        if (after.st_uid, after.st_gid) != (original.st_uid, original.st_gid):
            raise RuntimeError("host file ownership changed")
        if not (root / "var/lib/.rdshim-ownership").is_dir():
            raise RuntimeError("virtual ownership record missing")
        print("GUEST_FCHOWN_RESERVED_OK: file works; internal fd refused in both modes")


if __name__ == "__main__":
    main()
