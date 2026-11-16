"""Probe SDDM in a disposable Arch clone; never boot the installed root.

Set RD_SDDM_ROOT to a private clone under this checkout's build directory.
The clone must already contain the signed SDDM package, an enabled
display-manager.service, and the Wayland test configuration if KWin is expected.
This checks service/seat/process state, not visual pixels or authentication.
"""

import os
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from boot_process_scope import boot_processes

base = Path.cwd()
build = (base / "build").resolve()
root = Path(os.environ["RD_SDDM_ROOT"]).resolve(strict=True)
binary = Path(os.environ.get("RD_SDDM_BINARY", base / "shim_cli")).resolve(strict=True)
observe_seconds = float(os.environ.get("RD_SDDM_OBSERVE_SECONDS", "22"))
if not 5 <= observe_seconds <= 120:
    raise SystemExit("RD_SDDM_OBSERVE_SECONDS must be between 5 and 120")
if not root.is_relative_to(build) or not (root / "usr/bin/sddm").is_file():
    raise SystemExit("RD_SDDM_ROOT must be a disposable SDDM Arch clone under build/")
if not (root / "etc/systemd/system/display-manager.service").is_symlink():
    raise SystemExit("enable display-manager.service only in the disposable clone")

group = Path(tempfile.mkdtemp(prefix="shimmy-sddm-probe-", dir="/tmp"))
log = build / "sddm-probe-boot.log"
console = build / "sddm-probe-console.log"
console.unlink(missing_ok=True)
env = os.environ.copy()
env.update(
    RDUID0="1", RDNATIVE_SESSION="1", RDSOCKETGROUP=str(group),
    RDCONS_DUMP="1", RDCONS_DUMPFILE=str(console),
    RDENV_WAYLAND_DISPLAY="", RDENV_DISPLAY="", RDENV_XDG_RUNTIME_DIR="",
)


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def scoped():
    listing = subprocess.check_output(["ps", "-axo", "pid=,command="], text=True)
    return boot_processes(listing, binary, root)


proc = None
try:
    with log.open("w") as output:
        proc = subprocess.Popen(
            [str(binary), str(root), "/sbin/init", "--system"], env=env,
            stdout=output, stderr=output, start_new_session=True, preexec_fn=no_core,
        )
        # A greeter can start and crash before the final process snapshot. Record
        # that separately from a process that remains up long enough to be useful.
        kwin_seen = False
        kwin_first_seen = None
        kwin_longest_streak = 0.0
        kwin_streak_start = None
        deadline = time.monotonic() + observe_seconds
        while time.monotonic() < deadline:
            programs = [args.split(None, 1)[0] for _, args in scoped()]
            kwin_now = "/usr/bin/kwin_wayland" in programs
            now = time.monotonic()
            if kwin_now:
                kwin_seen = True
                if kwin_first_seen is None:
                    kwin_first_seen = now
                if kwin_streak_start is None:
                    kwin_streak_start = now
                kwin_longest_streak = max(kwin_longest_streak, now - kwin_streak_start)
            else:
                kwin_streak_start = None
            time.sleep(0.25)
        seat = subprocess.run(
            [str(binary), str(root), "/usr/bin/loginctl", "show-seat", "seat0",
             "-p", "CanTTY", "-p", "CanGraphical"],
            env=env, text=True, capture_output=True, timeout=8, preexec_fn=no_core,
        )
        programs = [args.split(None, 1)[0] for _, args in scoped()]
        scoped_processes = scoped()
        guest_processes = [args for _, args in scoped_processes]
        helper_pids = [str(pid) for pid, args in scoped_processes
                       if "/usr/lib/sddm/sddm-helper" in args]
        sddm_pids = [str(pid) for pid, args in scoped_processes
                     if args.split(None, 1)[0] == "/usr/bin/sddm"]
        helper_state = subprocess.run(
            ["ps", "-o", "pid,stat,wchan,command", "-p", ",".join(helper_pids)],
            text=True, capture_output=True, timeout=8,
        ) if helper_pids else None
        sessions = subprocess.run(
            [str(binary), str(root), "/usr/bin/loginctl", "list-sessions", "--no-legend"],
            env=env, text=True, capture_output=True, timeout=8, preexec_fn=no_core,
        )
        bus_names = subprocess.run(
            [str(binary), str(root), "/usr/bin/busctl", "--system", "list",
             "--no-pager", "--no-legend"],
            env=env, text=True, capture_output=True, timeout=8, preexec_fn=no_core,
        )
        runtime_dir = root / "run/user/962"
        runtime_stat = runtime_dir.stat() if runtime_dir.exists() else None
        sddm = "/usr/bin/sddm" in programs
        kwin = "/usr/bin/kwin_wayland" in programs
        print("SEAT_QUERY_EXIT", seat.returncode, flush=True)
        print("SEAT_PROPERTIES", seat.stdout.strip().replace("\n", " "), flush=True)
        print("SDDM_PROCESS", sddm, "KWIN_GREETER_PROCESS", kwin, flush=True)
        print("SDDM_PARENT_PIDS", sddm_pids, "SDDM_HELPER_PIDS", helper_pids, flush=True)
        print("KWIN_GREETER_SEEN", kwin_seen,
              "LONGEST_OBSERVED_SECONDS", round(kwin_longest_streak, 2), flush=True)
        print("GREETER_RELATED_PROCESSES", [args for args in guest_processes
              if any(name in args for name in ("sddm", "kwin", "logind", "user@"))],
              flush=True)
        print("LOGINCTL_SESSIONS", sessions.returncode, sessions.stdout.strip(), flush=True)
        print("DISPLAY_MANAGER_BUS_NAME", bus_names.returncode,
              [line for line in bus_names.stdout.splitlines()
               if "org.freedesktop.DisplayManager" in line], flush=True)
        print("SDDM_RUNTIME_DIR", None if runtime_stat is None else
              (runtime_stat.st_uid, runtime_stat.st_gid, oct(runtime_stat.st_mode & 0o7777)),
              flush=True)
        print("SDDM_HELPER_WAIT", None if helper_state is None else helper_state.stdout.strip(),
              flush=True)
        print("BOOT_LOG", log, flush=True)
        passed = (seat.returncode == 0 and "CanGraphical=yes" in seat.stdout
                  and sddm and kwin and kwin_longest_streak >= 5)
finally:
    # Re-exec'd Linux programs are separate host Shimmy processes. Scope every
    # termination to both this binary and this private root, never a live boot.
    for pid, _ in scoped():
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

sys.exit(0 if passed else 1)
