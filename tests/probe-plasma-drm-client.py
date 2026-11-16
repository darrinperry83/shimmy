#!/usr/bin/env python3
"""Isolated KWin DRM + foot windowed diagnostic; never uses scanout.

Requires a disposable Arch root with KWin, foot and a private /run/user/22001.
Only launches the explicitly supplied Shimmy binaries and cleans its own group.
"""

import argparse
import os
from pathlib import Path
import resource
import shutil
import signal
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--gui', type=Path, required=True)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=45)
    parser.add_argument('--logs', type=Path, required=True)
    args = parser.parse_args()
    if args.root.is_symlink():
        parser.error('--root must be a disposable directory, not a symlink to a live root')
    root, gui, cli = (p.resolve(strict=True) for p in (args.root, args.gui, args.cli))
    if not root.is_dir() or not all(p.is_file() and os.access(p, os.X_OK) for p in (gui, cli)):
        parser.error('root must be a directory and both shim binaries must be executable')
    if args.seconds < 5 or args.seconds > 300:
        parser.error('--seconds must be between 5 and 300')
    args.logs.mkdir(parents=True, exist_ok=True)
    group = Path(tempfile.mkdtemp(prefix='shim-plasma-probe-'))
    env = os.environ.copy()
    env.update(RDUID0='1', RDNATIVE_SESSION='1', RDSOCKETGROUP=str(group),
               RDNATIVE_MODE='windowed', RDENV_XDG_RUNTIME_DIR='/run/user/22001',
               RDENV_LIBSEAT_BACKEND='noop', RDENV_KWIN_DRM_DEVICES='/dev/dri/card0',
               RDENV_WAYLAND_DISPLAY='', RDENV_DISPLAY='')
    env.pop('RDGPU_SCANOUT', None)
    env.pop('RDGPU_SCANOUT_DIRECT_SOURCE', None)

    def preexec():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        os.setsid()

    kwin = foot = None
    try:
        with (args.logs / 'kwin.log').open('w') as kwlog, (args.logs / 'foot.log').open('w') as ftlog:
            kwin = subprocess.Popen([str(gui), str(root), '/usr/bin/kwin_wayland', '--drm',
                                     '--no-lockscreen'], env=env, stdout=kwlog,
                                    stderr=subprocess.STDOUT, preexec_fn=preexec)
            sock = group / 'run/user/22001/wayland-0'
            for _ in range(100):
                if sock.is_socket() or kwin.poll() is not None:
                    break
                time.sleep(.1)
            print(f'kwin_pid={kwin.pid} running={kwin.poll() is None} socket={sock.is_socket()}', flush=True)
            if not sock.is_socket():
                return 1
            footenv = env.copy()
            footenv['RDENV_WAYLAND_DISPLAY'] = 'wayland-0'
            foot = subprocess.Popen([str(cli), str(root), '/usr/bin/foot', '/bin/sh', '-c',
                                     f'sleep {args.seconds}'], env=footenv, stdout=ftlog,
                                    stderr=subprocess.STDOUT, preexec_fn=preexec)
            print(f'foot_pid={foot.pid} kwin_running={kwin.poll() is None}', flush=True)
            time.sleep(args.seconds)
            print(f'foot_exit={foot.poll()} kwin_exit={kwin.poll()}', flush=True)
            return 0 if kwin.poll() is None and (foot.poll() is None or foot.returncode == 0) else 1
    finally:
        for proc in (foot, kwin):
            if proc and proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
        for proc in (foot, kwin):
            if proc:
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
        shutil.rmtree(group)


if __name__ == '__main__':
    raise SystemExit(main())
