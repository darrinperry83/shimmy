"""Exercise the virtual DRM master in a private guest root and socket namespace.

Build tests/guest-drm-master.c for static aarch64 Linux and place it in private-root/tmp.
This harness never uses the installed rootfs or display.
"""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

base = Path.cwd()
root = Path(os.environ["RD_TEST_ROOT"]).resolve(strict=True)
if not root.is_relative_to((base / "build").resolve()):
    raise SystemExit("RD_TEST_ROOT must be a private clone under build/")
guest = root / "tmp/guest-drm-master"
if not guest.is_file():
    raise SystemExit("compile guest-drm-master.c into private-root/tmp first")

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="rdmaster-", dir="/tmp") as group:
    env = os.environ.copy()
    env.update(RDSOCKETGROUP=group)
    command = [str(base / "shim_cli"), str(root), "/tmp/guest-drm-master"]
    first = subprocess.Popen(command + ["hold"], env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, preexec_fn=no_core)
    try:
        line = first.stdout.readline().strip()
        print("OWNER", line, flush=True)
        if line != "SET r=0 errno=0":
            print("OWNER_ERROR", first.stderr.read()[-1200:], flush=True)
            raise AssertionError("first virtual DRM owner was not granted master")
        second = subprocess.run(command, env=env, capture_output=True, text=True,
                                timeout=15, preexec_fn=no_core)
        print("CONTENDER", second.stdout.strip(), flush=True)
        if "SET r=-1 errno=16" not in second.stdout:
            raise AssertionError("competing process was not denied EBUSY")
        remainder, err = first.communicate(timeout=15)
        print("OWNER_DONE", remainder.strip(), flush=True)
        if first.returncode or "DUP_SET r=0 errno=0" not in remainder or "DROP r=0 errno=0" not in remainder:
            raise AssertionError("owner duplicate/drop path failed")
        third = subprocess.run(command, env=env, capture_output=True, text=True,
                               timeout=15, preexec_fn=no_core)
        print("HANDOFF", third.stdout.strip(), flush=True)
        if "SET r=0 errno=0" not in third.stdout:
            raise AssertionError("master was not released for next owner")
    finally:
        if first.poll() is None:
            first.terminate()
            try:
                first.wait(timeout=3)
            except subprocess.TimeoutExpired:
                first.kill()
                first.wait()
