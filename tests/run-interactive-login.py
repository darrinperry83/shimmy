"""Type a throwaway account through the real getty/login pty in an isolated root.

Set RD_LOGIN_ROOT to a private clone under this checkout's build directory.
The live rootfs, account database, and personal credentials are never touched.
"""
import os
import struct
import resource
import secrets
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path
from boot_process_scope import boot_processes

base = Path.cwd()
root = Path(os.environ["RD_LOGIN_ROOT"]).resolve()
binary = Path(os.environ.get("RD_LOGIN_BINARY", str(base / "shim_cli"))).resolve(strict=True)
if not root.is_relative_to((base / "build").resolve()) or not (root / "usr/bin/login").is_file():
    raise SystemExit("RD_LOGIN_ROOT must be a private clone under build/")
if os.environ.get("RD_TEST_FRAME_PROGRESS") == "1" and \
        list((root / "run/user/22001").glob("wayland-*.lock")):
    raise SystemExit("stale Wayland lock in disposable root; start this probe with a fresh /run")
user = "shimtest"
existing = [line for line in (root / "etc/passwd").read_text().splitlines()
            if line.startswith(user + ":")]
if existing and (len(existing) != 1 or not existing[0].startswith(user + ":x:22001:22001:")):
    raise SystemExit("conflicting account in fixture")
for filename in ("passwd", "group", "shadow"):
    path = root / "etc" / filename
    lines = [line for line in path.read_text().splitlines()
             if not line.startswith(user + ":")]
    path.write_text("\n".join(lines) + "\n")

password = secrets.token_urlsafe(24)
hashed = subprocess.run(["openssl", "passwd", "-6", "-stdin"], input=password + "\n",
                        text=True, capture_output=True, check=True).stdout.strip()
with (root / "etc/passwd").open("a") as f:
    f.write(f"{user}:x:22001:22001:Shimmy test:/home/{user}:/bin/bash\n")
with (root / "etc/group").open("a") as f:
    f.write(f"{user}:x:22001:\n")
with (root / "etc/shadow").open("a") as f:
    f.write(f"{user}:{hashed}:20670::::::\n")
(root / "home" / user).mkdir(mode=0o700, exist_ok=True)
if os.environ.get("RD_TEST_FOOT") == "1":
    foot_seconds = 90 if os.environ.get("RD_TEST_STABILITY") == "1" else 20
    (root / "tmp/shim-foot-probe.sh").write_text(
        '#!/bin/sh\n'
        'for i in $(seq 1 60); do\n'
        '  for s in "$XDG_RUNTIME_DIR"/wayland-[0-9]*.lock; do\n'
        '    if [ -f "$s" ]; then\n'
        '      display="${s##*/}"\n'
        '      export WAYLAND_DISPLAY="${display%.lock}"\n'
        '      echo "FOOT_DISPLAY=$WAYLAND_DISPLAY"\n'
        f'      exec env WAYLAND_DEBUG=client /usr/bin/foot -e /usr/bin/sleep {foot_seconds}\n'
        '    fi\n'
        '  done\n'
        '  sleep 1\n'
        'done\n'
        'echo FOOT_NO_SOCKET\nexit 1\n')

# AF_UNIX paths are short; the socket namespace must not inherit the long checkout path.
tmp = Path(tempfile.mkdtemp(prefix="rdli-", dir="/tmp"))
fifo = tmp / "input"
os.mkfifo(fifo, 0o600)
console = base / "build/interactive-console.log"
log = base / "build/interactive-run.log"
console.unlink(missing_ok=True)
env = os.environ.copy()
env.update(RDUID0="1", RDCONS_DUMP="1", RDCONS_DUMPFILE=str(console),
           RDVT_TEST_INPUT_FIFO=str(fifo), RDSOCKETGROUP=str(tmp / "sockets"),
           RDNATIVE_SESSION="1", RDENV_LIBGL_ALWAYS_INDIRECT="",
           LIBGL_ALWAYS_INDIRECT="")
# Every invocation boots systemd: match run.sh --init even for the login-only
# probe, so build_guest_env omits the nested display/runtime defaults and
# PAM/logind owns the runtime directory. No pidfd or socket-queue opt-in is passed here.
(tmp / "sockets").mkdir(mode=0o700)

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

def wait_text(fragment, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            break
        if console.exists() and fragment in console.read_text(errors="replace"):
            return True
        time.sleep(0.2)
    return False

def type_keys(value):
    fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
    try:
        os.write(fd, value.encode())
    finally:
        os.close(fd)

ok = False
proc = None
try:
    with log.open("w") as output:
        proc = subprocess.Popen([str(binary), str(root), "/sbin/init", "--system"],
                                env=env, stdout=output, stderr=output,
                                start_new_session=True, preexec_fn=no_core)
        if not wait_text("login:", 90):
            print("LOGIN_PROMPT_MISSING", flush=True)
        else:
            print("LOGIN_PROMPT", flush=True)
            type_keys(user + "\n")
            password_wait = int(os.environ.get("RD_LOGIN_PASSWORD_WAIT", "20"))
            if not 1 <= password_wait <= 90:
                raise ValueError("RD_LOGIN_PASSWORD_WAIT must be 1..90 seconds")
            if not wait_text("Password:", password_wait):
                print("PASSWORD_PROMPT_MISSING", flush=True)
            else:
                print("PASSWORD_PROMPT", flush=True)
                type_keys(password + "\n")
                shell_wait = 60 if os.environ.get("RD_TEST_SESSION") == "1" else 25
                if wait_text("shimtest@", shell_wait) or wait_text("[shimtest@", 1):
                    type_keys("id -un; id -u\n")
                    ok = wait_text("\nshimtest\n", 8) and wait_text("\n22001\n", 3)
                print("LOGIN_SHELL_AND_ID" if ok else "LOGIN_SHELL_MISSING", flush=True)
                if ok and os.environ.get("RD_TEST_SEAT_DBUS") == "1":
                    type_keys("/tmp/shim-seat-dbus\n")
                    ok = wait_text("TakeDevice 226:0", 15)
                    if console.exists():
                        for line in console.read_text(errors="replace").splitlines():
                            if line.startswith(("device r=", "devname r=", "direct logind-flags",
                                                "open bus r=", "TakeControl r=", "TakeDevice ", "ReadReply ")):
                                print(line, flush=True)
                if ok and os.environ.get("RD_TEST_PLASMA") == "1":
                    if os.environ.get("RD_TEST_DRM_TRACE") == "1":
                        type_keys("systemctl --user set-environment RDDRM_TRACE=1; echo DRM_TRACE_SET\n")
                        if not wait_text("DRM_TRACE_SET", 8):
                            raise RuntimeError("could not set user-manager DRM trace")
                    type_keys("printf 'PLASMA_RUNTIME=%s\\n' \"$XDG_RUNTIME_DIR\"\n")
                    runtime_seen = wait_text("PLASMA_RUNTIME=/run/user/22001", 8)
                    if os.environ.get("RD_TEST_FOOT") == "1":
                        type_keys("sh /tmp/shim-foot-probe.sh >/tmp/shim-foot-probe.log 2>&1 &\n")
                    type_keys("/usr/lib/plasma-dbus-run-session-if-needed /usr/bin/startplasma-wayland\n")
                    hold = int(os.environ.get("RD_TEST_PLASMA_HOLD", "15"))
                    if not 1 <= hold <= 120:
                        raise ValueError("RD_TEST_PLASMA_HOLD must be 1..120 seconds")
                    def plasma_pids():
                        rows = subprocess.check_output(["ps", "-axo", "pid=,command="], text=True)
                        return {args.split(None, 1)[0]: int(pid)
                                for pid, args in boot_processes(rows, binary, root)
                                if args.split(None, 1)[0] in ("/usr/bin/kwin_wayland", "/usr/bin/plasmashell", "/usr/bin/foot")}
                    first = None
                    if hold > 5:
                        first_delay = min(20, hold // 2) if os.environ.get("RD_TEST_STABILITY") == "1" else 5
                        time.sleep(first_delay)
                        first = plasma_pids()
                        time.sleep(hold - first_delay)
                    else:
                        time.sleep(hold)
                    last = plasma_pids()
                    print("PLASMA_PIDS_FIRST", first, flush=True)
                    print("PLASMA_PIDS_LAST", last, flush=True)
                    print("KWIN_SAME_PID" if first and first.get("/usr/bin/kwin_wayland") ==
                          last.get("/usr/bin/kwin_wayland") else "KWIN_PID_CHANGED", flush=True)
                    if os.environ.get("RD_TEST_FOOT") == "1":
                        print("FOOT_SAME_PID" if first and first.get("/usr/bin/foot") and
                              first.get("/usr/bin/foot") == last.get("/usr/bin/foot") else
                              "FOOT_PID_CHANGED", flush=True)
                    listing = subprocess.check_output(["ps", "-axo", "pid=,command="], text=True)
                    programs = [args.split(None, 1)[0]
                                for _, args in boot_processes(listing, binary, root)]
                    print("PLASMA_RUNTIME_OK" if runtime_seen else "PLASMA_RUNTIME_MISSING", flush=True)
                    print("PLASMA_PROGRAMS", ",".join(sorted(set(programs))), flush=True)
                    if os.environ.get("RD_TEST_FOOT") == "1":
                        probe = root / "tmp/shim-foot-probe.log"
                        print("FOOT_PROBE", probe.read_text(errors="replace")[-2000:] if probe.exists() else "missing", flush=True)
                    # This is only a process-start probe. A visible desktop and a working
                    # greeter/logout cycle require a separate GUI integration test.
                    kwin_seen = "/usr/bin/kwin_wayland" in programs
                    shell_seen = "/usr/bin/plasmashell" in programs
                    print("KWIN_PROCESS_SEEN" if kwin_seen else "KWIN_PROCESS_MISSING", flush=True)
                    print("PLASMASHELL_PROCESS_SEEN" if shell_seen else "PLASMASHELL_PROCESS_MISSING", flush=True)
                    ok = runtime_seen and kwin_seen and shell_seen
                    if os.environ.get("RD_TEST_FRAME_PROGRESS") == "1":
                        # The CPU frame carrier is a testable output of KWin's DRM flips.
                        # More than two frames with real pixels rules out the previous
                        # permanent Xwayland-read wait and frozen black framebuffer.
                        frame_ok = False
                        frame_dir = Path(f"/tmp/rdx-{proc.pid}")
                        for frame in frame_dir.glob("frame.*.*.*"):
                            try:
                                with frame.open("rb") as source:
                                    seq, magic, width, height, crtc = struct.unpack("<QIIII", source.read(24))
                                    if magic != 0x52444652 or seq < 6 or seq & 1 or not 0 < width <= 8192 or not 0 < height <= 8192:
                                        continue
                                    remaining = width * height * 4
                                    nonblack = False
                                    while remaining:
                                        chunk = source.read(min(65536, remaining))
                                        if not chunk:
                                            break
                                        if any(chunk[0::4]) or any(chunk[1::4]) or any(chunk[2::4]):
                                            nonblack = True
                                            break
                                        remaining -= len(chunk)
                                    if nonblack:
                                        print(f"DRM_FRAME_PROGRESS seq={seq} size={width}x{height} crtc={crtc}", flush=True)
                                        frame_ok = True
                                        break
                            except (OSError, struct.error):
                                continue
                        print("DRM_FRAME_PROGRESS_OK" if frame_ok else "DRM_FRAME_PROGRESS_MISSING", flush=True)
                        xwayland_seen = "/usr/bin/Xwayland" in programs
                        print("XWAYLAND_PROCESS_SEEN" if xwayland_seen else "XWAYLAND_PROCESS_MISSING", flush=True)
                        ok = ok and frame_ok and xwayland_seen
                if ok and os.environ.get("RD_TEST_SESSION") == "1":
                    type_keys("printf 'SHIMMY_RUNTIME=%s\\n' \"$XDG_RUNTIME_DIR\"; "
                              "systemctl --user is-active basic.target && "
                              "printf 'SHIMMY_USER_%s\\n' MANAGER_READY\n")
                    runtime = wait_text("SHIMMY_RUNTIME=/run/user/22001", 12)
                    manager = wait_text("SHIMMY_USER_MANAGER_READY", 12)
                    ok = runtime and manager
                    if ok:
                        type_keys("stat -c 'SHIMMY_RUNTIME_OWNER=%u:%g:%a' /run/user/22001\n")
                        ok = wait_text("SHIMMY_RUNTIME_OWNER=22001:22001:700", 8)
                        print("LOGIN_RUNTIME_OWNER_OK" if ok else "LOGIN_RUNTIME_OWNER_BAD", flush=True)
                    print("LOGIN_USER_SESSION_OK" if ok else "LOGIN_USER_SESSION_MISSING", flush=True)
                    if ok:
                        type_keys("busctl --user list >/dev/null && "
                                  "printf 'SHIMMY_USER_BUS_%s\\n' READY\n")
                        ok = wait_text("SHIMMY_USER_BUS_READY", 12)
                        print("LOGIN_USER_BUS_OK" if ok else "LOGIN_USER_BUS_MISSING", flush=True)
                if ok and os.environ.get("RD_TEST_LOGOUT") == "1":
                    before = len(console.read_text(errors="replace"))
                    type_keys("exit\n")
                    deadline = time.monotonic() + 15
                    while time.monotonic() < deadline:
                        if "localhost login:" in console.read_text(errors="replace")[before:]:
                            break
                        time.sleep(0.2)
                    else:
                        ok = False
                    print("LOGIN_LOGOUT_OK" if ok else "LOGIN_LOGOUT_MISSING", flush=True)
                if ok and os.environ.get("RD_TEST_JOB_CONTROL") == "1":
                    if console.exists() and "no job control in this shell" in console.read_text(errors="replace"):
                        ok = False
                    else:
                        type_keys("sleep 20\n")
                        time.sleep(1)
                        type_keys("\x03")
                        type_keys("printf '\\nINT_DONE\\n'\n")
                        interrupted = wait_text("\nINT_DONE\n", 6)
                        type_keys("sleep 20\n")
                        time.sleep(1)
                        type_keys("\x1a")
                        type_keys("jobs\n")
                        stopped = wait_text("Stopped", 6)
                        type_keys("printf '\\nSTOP_DONE\\n'\n")
                        resumed = wait_text("\nSTOP_DONE\n", 6)
                        ok = interrupted and stopped and resumed
                    print("JOB_CONTROL_OK" if ok else "JOB_CONTROL_FAILED", flush=True)
finally:
    # Exact binary + private root match prevents interference with a live Shimmy session.
    listing = subprocess.check_output(["ps", "-axo", "pid=,command="], text=True)
    for pid, _ in boot_processes(listing, binary, root):
        try: os.kill(pid, signal.SIGKILL)
        except ProcessLookupError: pass
    if proc is not None:
        try: proc.wait(timeout=3)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    shutil.rmtree(tmp)
    if console.exists():
        print("console tail:", console.read_text(errors="replace")[-1000:], flush=True)
raise SystemExit(0 if ok else 1)
