#!/usr/bin/env python3
"""Probe checked fchownat guest paths without changing host file ownership."""
from pathlib import Path
import os
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-fchownat-path-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        (root / "var/lib").mkdir(parents=True)
        file = root / "tmp/fchownat-path-file"
        file.write_bytes(b"fixture")
        original = file.stat()
        for mode in ("virtual", "host"):
            guest = root / f"tmp/guest-fchownat-{mode}"
            compile_args = [
                "clang", "--target=aarch64-linux-gnu", "-nostdlib",
                "-static-pie", "-fuse-ld=lld", "-Wl,-e,_start",
            ]
            if mode == "host":
                compile_args.append("-DHOST_ONLY")
            compile_args += [str(repo / "tests/guest-fchownat-path.c"),
                             "-o", str(guest)]
            subprocess.run(compile_args, check=True, timeout=30)
            env = os.environ.copy()
            env["RDUID0"] = "1" if mode == "virtual" else "0"
            result = subprocess.run([
                str((repo / "shim_cli").resolve(strict=True)), str(root),
                f"/tmp/guest-fchownat-{mode}"], env=env, capture_output=True,
                text=True, timeout=30, preexec_fn=no_core)
            if result.returncode or "GUEST_FCHOWNAT_PATH_OK" not in result.stdout:
                raise RuntimeError(f"{mode} guest failed rc={result.returncode}\n"
                                   f"{result.stdout}\n{result.stderr[-2000:]}")
        after = file.stat()
        if (after.st_uid, after.st_gid) != (original.st_uid, original.st_gid):
            raise RuntimeError("host file ownership changed")
        print("GUEST_FCHOWNAT_PATH_OK: both modes reject bad pointers; virtual paths work")


if __name__ == "__main__":
    main()
