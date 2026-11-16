"""Type a throwaway account through the real getty/login pty in an isolated root.

Set RD_LOGIN_ROOT to a private clone under this checkout's build directory.
The live rootfs, account database, and personal credentials are never touched.
"""
import os
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
binary = (base / "shim_cli").resolve(strict=True)
if not root.is_relative_to((base / "build").resolve()) or not (root / "usr/bin/login").is_file():
    raise SystemExit("RD_LOGIN_ROOT must be a private clone under build/")
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

# AF_UNIX paths are short; the socket namespace must not inherit the long checkout path.
tmp = Path(tempfile.mkdtemp(prefix="rdli-", dir="/tmp"))
fifo = tmp / "input"
os.mkfifo(fifo, 0o600)
console = base / "build/interactive-console.log"
log = base / "build/interactive-run.log"
console.unlink(missing_ok=True)
env = os.environ.copy()
env.update(RDUID0="1", RDCONS_DUMP="1", RDCONS_DUMPFILE=str(console),
           RDVT_TEST_INPUT_FIFO=str(fifo), RDSOCKETGROUP=str(tmp / "sockets"))
if os.environ.get("RD_TEST_PLASMA") == "1":
    # Match run.sh --init: the display manager/session establishes these itself.
    env.update(RDNATIVE_SESSION="1", RDENV_WAYLAND_DISPLAY="",
               RDENV_DISPLAY="", RDENV_XDG_RUNTIME_DIR="",
               RDENV_LIBGL_ALWAYS_INDIRECT="", LIBGL_ALWAYS_INDIRECT="")
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
            if not wait_text("Password:", 20):
                print("PASSWORD_PROMPT_MISSING", flush=True)
            else:
                print("PASSWORD_PROMPT", flush=True)
                type_keys(password + "\n")
                shell_wait = 60 if os.environ.get("RD_TEST_SESSION") == "1" else 25
                if wait_text("shimtest@", shell_wait) or wait_text("[shimtest@", 1):
                    type_keys("id -un; id -u\n")
                    ok = wait_text("\nshimtest\n", 8) and wait_text("\n22001\n", 3)
                print("LOGIN_SHELL_AND_ID" if ok else "LOGIN_SHELL_MISSING", flush=True)
                if ok and os.environ.get("RD_TEST_PLASMA") == "1":
                    type_keys("printf 'PLASMA_RUNTIME=%s\\n' \"$XDG_RUNTIME_DIR\"\n")
                    runtime_seen = wait_text("PLASMA_RUNTIME=/run/user/22001", 8)
                    type_keys("/usr/lib/plasma-dbus-run-session-if-needed /usr/bin/startplasma-wayland\n")
                    time.sleep(15)
                    listing = subprocess.check_output(["ps", "-axo", "pid=,command="], text=True)
                    programs = [args.split(None, 1)[0]
                                for _, args in boot_processes(listing, binary, root)]
                    print("PLASMA_RUNTIME_OK" if runtime_seen else "PLASMA_RUNTIME_MISSING", flush=True)
                    print("PLASMA_PROGRAMS", ",".join(sorted(set(programs))), flush=True)
                    # This is only a process-start probe. A visible desktop and a working
                    # greeter/logout cycle require a separate GUI integration test.
                    kwin_seen = any("kwin_wayland" in p for p in programs)
                    print("KWIN_PROCESS_SEEN" if kwin_seen else "KWIN_PROCESS_MISSING", flush=True)
                    ok = runtime_seen and kwin_seen
                if ok and os.environ.get("RD_TEST_SESSION") == "1":
                    type_keys("printf 'SHIMMY_RUNTIME=%s\\n' \"$XDG_RUNTIME_DIR\"; "
                              "systemctl --user is-active basic.target && "
                              "printf 'SHIMMY_USER_%s\\n' MANAGER_READY\n")
                    runtime = wait_text("SHIMMY_RUNTIME=/run/user/22001", 12)
                    manager = wait_text("SHIMMY_USER_MANAGER_READY", 12)
                    ok = runtime and manager
                    print("LOGIN_USER_SESSION_OK" if ok else "LOGIN_USER_SESSION_MISSING", flush=True)
                    if ok:
                        type_keys("busctl --user list >/dev/null && "
                                  "printf 'SHIMMY_USER_BUS_%s\\n' READY\n")
                        ok = wait_text("SHIMMY_USER_BUS_READY", 12)
                        print("LOGIN_USER_BUS_OK" if ok else "LOGIN_USER_BUS_MISSING", flush=True)
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
