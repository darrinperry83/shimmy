#!/usr/bin/env python3
"""Build the init lifecycle gate; reference Linux results never qualify XNU."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / 'out/init'
BASE = 'sha256:0151e70bd01e7e4c891e903edf1dacba81656f04c6150bfce78d5a570e4df443'


def run(args, timeout=90, check=True):
    result = subprocess.run(list(map(str, args)), text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout)
    if check and result.returncode:
        raise RuntimeError(f'{args[0]} exited {result.returncode}: {result.stdout}')
    return result


def reference(mode):
    name = 'rdxnu-init-reference-' + uuid.uuid4().hex[:12]
    command = ['/capture/init-probe']
    if mode == 'init-death':
        command.append('exit-with-child')
    if mode == 'not-init':
        # Shell remains PID 1 while the ELF runs as a normal child.
        command = ['sh', '-c', '/capture/init-probe; result=$?; exit "$result"']
    if mode == 'subreaper':
        command = ['sh', '-c', '/capture/init-probe subreaper-only; result=$?; exit "$result"']
    if mode == 'native-parent':
        command = ['sh', '-c', 'cc -O2 -Wall -Wextra -Werror /input/run.c -o /tmp/init-run && /tmp/init-run /capture/init-probe']
    capabilities = ['--cap-add', 'SETUID', '--cap-add', 'SETGID'] if mode == 'native-parent' else []
    try:
        result = run(['docker', 'run', '--name', name, '--network', 'none',
                      '--read-only', '--cap-drop', 'ALL', *capabilities, '--pids-limit', '64',
                      '--tmpfs', '/tmp:exec', '-v', f'{HERE}:/input:ro',
                      '-v', f'{OUT}:/capture:ro', BASE, *command], check=False)
        state = json.loads(run(['docker', 'inspect', name]).stdout)[0]['State']
        if state['Running'] or state['OOMKilled']:
            raise RuntimeError('reference did not stop cleanly')
        expected = 'XNU_SUBREAPER_PERSONALITY_PASS' if mode == 'native-parent' else 'LINUX_SUBREAPER_PASS' if mode == 'subreaper' else 'INIT_CHECK SC(172' if mode == 'not-init' else (
            'LINUX_INIT_EXIT_WITH_LIVE_CHILD' if mode == 'init-death' else 'LINUX_INIT_LIFECYCLE_PASS')
        if expected not in result.stdout or result.returncode != (160 if mode == 'not-init' else 0):
            raise RuntimeError(f'{mode}: exit={result.returncode}\n{result.stdout}')
        (OUT / f'{mode}.log').write_text(result.stdout)
        return dict(mode=mode, exit_status=result.returncode, output=result.stdout,
                    container_stopped=True, oom_killed=False)
    finally:
        run(['docker', 'rm', '-f', name], check=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux-reference', action='store_true')
    args = parser.parse_args()
    OUT.mkdir(exist_ok=True)
    run(['xcrun', 'clang', '--target=aarch64-linux-gnu', '-ffreestanding',
         '-fno-builtin', '-fno-stack-protector', '-ffixed-x18', '-O2',
         '-Wall', '-Wextra', '-Werror', '-c', HERE / 'probe.c', '-o', OUT / 'init-probe.o'])
    run(['ld.lld', '-m', 'aarch64elf', '-z', 'max-page-size=16384', '--build-id=none',
         '-static', '-e', '_start', OUT / 'init-probe.o', '-o', OUT / 'init-probe'])
    run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', HERE / 'run.c', '-o', OUT / 'init-run'])
    report = dict(linux_elf_sha256=hashlib.sha256((OUT / 'init-probe').read_bytes()).hexdigest(),
                  xnu_init_domain_pass=False, xnu_full_init_boot_pass=False)
    if args.linux_reference:
        report.update(linux_image=BASE, reference=[reference(mode) for mode in
                      ('lifecycle', 'not-init', 'subreaper', 'native-parent', 'init-death', 'lifecycle')])
    (OUT / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
