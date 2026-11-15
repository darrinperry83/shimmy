"""Private Arch fixture smoke test: agetty autologin reaches a shell without credentials."""
import os, resource, signal, subprocess, time
from pathlib import Path

base = Path.cwd()
root = base / 'tests/login-root'
override = root / 'etc/systemd/system/getty@tty1.service.d/90-test-autologin.conf'
if override.exists():
    raise SystemExit(f'refusing to overwrite {override}')
console = base / 'build/autologin-console.log'
log = base / 'build/autologin-run.log'
console.unlink(missing_ok=True)
override.write_text('[Service]\nExecStart=\nExecStart=-/usr/bin/agetty --autologin root --noreset --noclear - ${TERM}\n')
env = os.environ.copy()
env.update(RDUID0='1', RDCONS_DUMP='1', RDCONS_DUMPFILE=str(console),
           RDSOCKETGROUP='/tmp/rd-autologin-test')

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

found = False
p = None
try:
    with log.open('w') as out:
        p = subprocess.Popen(['./shim_cli_login_candidate', 'tests/login-root',
                              '/sbin/init', '--system'], env=env, stdout=out,
                             stderr=out, start_new_session=True, preexec_fn=no_core)
        for _ in range(480):
            time.sleep(.25)
            if p.poll() is not None:
                break
            if not console.exists():
                continue
            data = console.read_text(errors='replace')
            if 'root@localhost' in data or 'localhost ~]#' in data or 'bash-' in data:
                found = True
                print('AUTOLOGIN_SHELL', flush=True)
                break
        print('console tail:', console.read_text(errors='replace')[-800:] if console.exists() else '(none)')
finally:
    listing = subprocess.check_output(['ps', '-axo', 'pid=,command='], text=True)
    for line in listing.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) == 2 and parts[1].startswith('./shim_cli_login_candidate tests/login-root '):
            try: os.kill(int(parts[0]), signal.SIGKILL)
            except ProcessLookupError: pass
    if p is not None:
        try: p.wait(timeout=3)
        except subprocess.TimeoutExpired: p.kill(); p.wait()
    override.unlink()
raise SystemExit(0 if found else 1)
