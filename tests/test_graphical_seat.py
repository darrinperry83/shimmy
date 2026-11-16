"""Check the boot owner's virtual DRM udev seat identity in a disposable Arch root.

First boot the private clone with RDDRM=1. Set RD_SEAT_ROOT to that clone under
this checkout's build directory. A standalone RDDRM=0 tool must not remove
the active boot owner's seat tags.
Never point this test at a live or shared guest root.
"""

import os
from pathlib import Path
import resource
import subprocess

base = Path.cwd()
root = Path(os.environ["RD_SEAT_ROOT"]).resolve(strict=True)
binary = (base / "shim_cli").resolve(strict=True)
if not root.is_relative_to((base / "build").resolve()) or not (root / "usr/bin/udevadm").exists():
    raise SystemExit("RD_SEAT_ROOT must be a private Arch clone under build/")


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def tagged_card(enabled: bool) -> str:
    env = os.environ.copy()
    env.update(RDUID0="1", RDDRM="1" if enabled else "0")
    run = subprocess.run(
        [str(binary), str(root), "/usr/bin/udevadm", "info", "--export-db",
         "--tag-match=master-of-seat"],
        env=env, capture_output=True, text=True, timeout=15, preexec_fn=no_core,
    )
    if run.returncode:
        raise AssertionError(run.stderr[-1000:])
    return run.stdout


def guest(*args: str) -> str:
    env = os.environ.copy()
    env.update(RDUID0="1", RDDRM="1")
    run = subprocess.run([str(binary), str(root), *args], env=env,
                         capture_output=True, text=True, timeout=15, preexec_fn=no_core)
    if run.returncode:
        raise AssertionError(run.stderr[-1000:])
    return run.stdout


positive = tagged_card(True)
assert "P: /class/drm/card0" in positive
assert "E: CURRENT_TAGS=" in positive
assert "master-of-seat" in positive
assert (root / "sys/dev/char/226:0").is_symlink()
assert (root / "sys/dev/char/226:128").is_symlink()
assert (root / "run/udev/data/c226:128").exists()
for node, minor in (("card0", "0"), ("renderD128", "128")):
    props = guest("/usr/bin/udevadm", "info", "--query=property", f"--name=/dev/dri/{node}")
    assert f"DEVNAME=/dev/dri/{node}\n" in props
    assert "MAJOR=226\n" in props and f"MINOR={minor}\n" in props
    assert "SUBSYSTEM=drm\n" in props
    devnum = guest("/usr/bin/stat", "-c", "%t:%T", f"/dev/dri/{node}").strip()
    assert devnum == ("e2:0" if minor == "0" else "e2:80")

other_process = tagged_card(False)
assert "P: /class/drm/card0" in other_process
assert (root / "run/udev/tags/master-of-seat/c226:0").exists()
print("virtual DRM udev seat identity and non-owner preservation passed")
