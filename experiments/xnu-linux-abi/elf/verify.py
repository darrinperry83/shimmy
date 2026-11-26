#!/usr/bin/env python3
"""Build/test the M1 ELF and optionally compare it on real Linux via Docker."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / 'out'


def run(command):
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f'{command!r}:\n{result.stdout}{result.stderr}')
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux-reference', action='store_true')
    args = parser.parse_args()
    OUT.mkdir(exist_ok=True)
    run(['xcrun', 'clang', '--target=aarch64-linux-gnu', '-c', str(HERE / 'probe.S'),
         '-o', str(OUT / 'elf-probe.o')])
    run(['ld.lld', '-m', 'aarch64elf', '-z', 'max-page-size=16384', '--build-id=none',
         '-static', '-e', '_start', str(OUT / 'elf-probe.o'), '-o', str(OUT / 'elf-probe')])
    run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', str(HERE / 'run_probe.c'),
         '-o', str(OUT / 'elf-run-probe')])
    run(['xcrun', 'clang', '-fsanitize=address,undefined', '-g', '-Wall', '-Wextra', '-Werror',
         str(HERE / 'test_plan.c'), '-o', str(OUT / 'elf-test-plan')])
    report = dict(elf_sha256=hashlib.sha256((OUT / 'elf-probe').read_bytes()).hexdigest(),
                  planner=run([str(OUT / 'elf-test-plan'), str(OUT / 'elf-probe')]).strip(),
                  xnu_execution_proven=False)
    if args.linux_reference:
        image = run(['docker', 'image', 'inspect', 'menci/archlinuxarm:base-devel',
                     '--format', '{{.Id}}']).strip()
        output = run(['docker', 'run', '--rm', '--network', 'none', '-v',
            f'{HERE.parent}:/work:ro', image, 'sh', '-c',
            'uname -a; cc -O2 -Wall -Wextra -Werror /work/elf/run_probe.c -o /tmp/elf-run; '
            '/tmp/elf-run /work/out/elf-probe'])
        if 'LINUX_ELF_M1_PASS' not in output:
            raise RuntimeError('Linux reference did not pass')
        report.update(linux_reference=output, linux_image=image)
    (OUT / 'elf-verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
