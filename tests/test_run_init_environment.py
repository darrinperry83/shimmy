"""Check the host-to-guest environment contract for a normal init launch.

The fake shim keeps this test off the user's running root and GUI. Native
build_guest_env() omits the nested display and runtime defaults.
"""

import json
import os
from pathlib import Path
import subprocess
import tempfile


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="shimmy-init-env-") as tmp:
    base = Path(tmp)
    root = base / "root"
    (root / "tmp").mkdir(parents=True)
    (root / "tmp").chmod(0o1777)
    (root / "sbin").mkdir()
    init = root / "sbin/init"
    init.write_text("#!/bin/sh\nexit 0\n")
    init.chmod(0o755)

    fake = base / "fake-shim"
    fake.write_text(
        "#!/usr/bin/env python3\n"
        "import json, os, sys\n"
        "print(json.dumps({'argv': sys.argv[1:], 'env': "
        "{k: os.environ.get(k) for k in "
        "['RDUID0', 'RDNATIVE_SESSION', 'LIBGL_ALWAYS_INDIRECT', "
        "'RDENV_WAYLAND_DISPLAY', 'RDENV_DISPLAY', "
        "'RDENV_XDG_RUNTIME_DIR', 'RDENV_LIBGL_ALWAYS_INDIRECT']}}))\n"
    )
    fake.chmod(0o755)

    env = os.environ.copy()
    env.update(ROOTFS=str(root), SHIM_GUI_OVERRIDE=str(fake), LIBGL_ALWAYS_INDIRECT="1")
    run = subprocess.run([str(repo / "run.sh"), "--init"], env=env,
                         text=True, capture_output=True, check=True)
    result = json.loads(run.stdout)
    assert result["argv"] == [str(root), "/sbin/init"], result
    assert result["env"] == {
        "RDUID0": "1",
        "RDNATIVE_SESSION": "1",
        "LIBGL_ALWAYS_INDIRECT": "1",  # host value; real shim drops guest copy
        "RDENV_WAYLAND_DISPLAY": None,
        "RDENV_DISPLAY": None,
        "RDENV_XDG_RUNTIME_DIR": None,
        "RDENV_LIBGL_ALWAYS_INDIRECT": "",
    }, result
    for tty in range(2, 7):
        link = root / f"etc/systemd/system/getty.target.wants/getty@tty{tty}.service"
        assert link.is_symlink(), link

print("normal init launch leaves display/session variables to Arch")
