"""Boot a private Arch clone and verify a fresh login plus settled system bus."""

import os
from pathlib import Path
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from boot_process_scope import boot_processes

base = Path.cwd()
build = (base / "build").resolve()
given_root = os.environ.get("RD_LOGIN_ROOT")
if not given_root:
    raise SystemExit("Set RD_LOGIN_ROOT to a private Arch clone under build/")
root = Path(given_root).resolve(strict=True)
binary = Path(os.environ.get("RD_LOGIN_BINARY", "./shim_cli")).resolve(strict=True)
if not root.is_relative_to(build) or not (root / "sbin/init").exists():
    raise SystemExit("RD_LOGIN_ROOT must be a private bootable clone under build/")

group = Path(tempfile.mkdtemp(prefix="rd-login-", dir="/tmp"))
console = build / "init-console6.log"
log = build / "init-run6.log"
console.unlink(missing_ok=True)  # Never accept a prompt left by an earlier run.
env = os.environ.copy()
env.update(RDUID0="1", RDCONS_DUMP="1", RDCONS_DUMPFILE=str(console),
           RDSOCKETGROUP=str(group))


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def scoped_processes():
    listing = subprocess.check_output(["ps", "-axo", "pid=,command="], text=True)
    return boot_processes(listing, binary, root)


found = False
bus_settled = False
proc = None
try:
    with log.open("w") as out:
        proc = subprocess.Popen([str(binary), str(root), "/sbin/init", "--system"],
                                env=env, stdout=out, stderr=out,
                                start_new_session=True, preexec_fn=no_core)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline and proc.poll() is None:
            time.sleep(.25)
            if console.exists() and "login:" in console.read_text(errors="replace"):
                found = True
                print("LOGIN prompt captured", flush=True)
                time.sleep(float(os.environ.get("RD_LOGIN_SETTLE_SECONDS", "3")))
                programs = [args.split(None, 1)[0] for _, args in scoped_processes()]
                bus_settled = ("/usr/bin/dbus-broker" in programs and
                               "/usr/bin/dbus-broker-launch" in programs)
                print("D-Bus broker and launcher settled:", bus_settled, flush=True)
                break
finally:
    # Re-exec'd guests use absolute binary/root paths and may create new host
    # sessions. Match BOTH identities before stopping any process.
    for pid, _ in scoped_processes():
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    if proc is not None:
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    shutil.rmtree(group)

print("init test PID", proc.pid if proc else None,
      "exit", proc.returncode if proc else None, flush=True)
sys.exit(0 if found and bus_settled else 1)
