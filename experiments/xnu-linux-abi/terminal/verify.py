#!/usr/bin/env python3
"""Build the unchanged Linux read fixture and native PTY parent; optional Linux run."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / 'out/terminal'


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'{command!r}:\n{result.stdout}{result.stderr}')
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux-reference', action='store_true')
    args = parser.parse_args()
    OUT.mkdir(exist_ok=True)
    run(['xcrun', 'clang', '--target=aarch64-linux-gnu', '-c', str(HERE / 'read_probe.S'),
         '-o', str(OUT / 'read-probe.o')])
    run(['ld.lld', '-m', 'aarch64elf', '-z', 'max-page-size=16384', '--build-id=none',
         '-static', '-e', '_start', str(OUT / 'read-probe.o'), '-o', str(OUT / 'read-probe')])
    run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', str(HERE / 'run_pty.c'),
         '-o', str(OUT / 'run-pty')])
    report = {'linux_elf_sha256': hashlib.sha256((OUT / 'read-probe').read_bytes()).hexdigest(),
              'native_parent_built': True, 'xnu_runtime_proven': False,
              'interactive_shell_proven': False}
    if args.linux_reference:
        image = run(['docker', 'image', 'inspect', 'menci/archlinuxarm:base-devel',
                     '--format', '{{.Id}}']).strip()
        output = run(['docker', 'run', '--rm', '--network', 'none', '-v',
                      f'{HERE.parent}:/work:ro', image, 'sh', '-c',
                      'cc -O2 -Wall -Wextra -Werror /work/terminal/run_pty.c -lutil -o /tmp/run-pty && '
                      '/tmp/run-pty /work/out/terminal/read-probe'])
        if 'LINUX_PTY_READ_PASS' not in output:
            raise RuntimeError('missing Linux PTY success marker')
        report.update(linux_image=image, linux_reference=output)
    (OUT / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
