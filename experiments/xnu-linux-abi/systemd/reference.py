#!/usr/bin/env python3
"""Capture the exact Arch systemd target and its real Linux startup syscalls.

Docker is a reference-test tool only. This does not run systemd on XNU or as
PID 1, and the resulting report deliberately keeps those gates unproven.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / 'out/systemd'


def run(argv, timeout=90):
    result = subprocess.run(list(map(str, argv)), text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{argv[0]} exited {result.returncode}:\n{result.stdout}')
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', default='menci/archlinuxarm:base-devel')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    image = run(['docker', 'image', 'inspect', args.image, '--format', '{{.Id}}']).strip()
    # Trace the unchanged loader explicitly: this is also the entry route that
    # the existing XNU ELF checkpoint supports (PT_INTERP is a later gate).
    (OUT / 'trace.gdb').write_text('''set pagination off
set confirm off
set startup-with-shell off
catch syscall
commands
silent
printf "RDXNU_SYSCALL %lu %lu\\n", $x8, $x0
continue
end
run --inhibit-cache --library-path /capture/rootfs/usr/lib/systemd:/capture/rootfs/usr/lib /capture/rootfs/usr/lib/systemd/systemd --version
''')
    script = r'''
set -eu
# This directory contains only this tool's generated binary closure. Avoid
# retaining libraries from a previous image when capturing an update.
rm -rf /capture/rootfs
mkdir -p /capture/rootfs
pacman -Q systemd glibc gcc-libs > /capture/packages.txt
ldd /usr/lib/systemd/systemd > /capture/dependencies.txt
# Copy the resolved closure with its absolute directory layout. ldd's vdso
# line is intentionally excluded; an ELF interpreter is copied explicitly.
{ printf '%s\n' /usr/lib/systemd/systemd /usr/lib/ld-linux-aarch64.so.1;
  awk '/=> \/.* \(/ { print $3 }' /capture/dependencies.txt;
} | sort -u | while IFS= read -r path; do
    cp --parents -L "$path" /capture/rootfs
done
cp /usr/share/gdb/syscalls/aarch64-linux.xml /capture/syscalls.xml
/usr/lib/systemd/systemd --version > /capture/linux-direct-version.log
/capture/rootfs/usr/lib/ld-linux-aarch64.so.1 --inhibit-cache \
  --library-path /capture/rootfs/usr/lib/systemd:/capture/rootfs/usr/lib \
  /capture/rootfs/usr/lib/systemd/systemd --version > /capture/linux-bundle-version.log
cmp /capture/linux-direct-version.log /capture/linux-bundle-version.log
gdb -q -nx -batch -x /capture/trace.gdb \
  /capture/rootfs/usr/lib/ld-linux-aarch64.so.1 > /capture/linux-syscalls.log 2>&1
'''
    run(['docker', 'run', '--rm', '--network', 'none', '--cap-add', 'SYS_PTRACE',
         '-v', f'{OUT}:/capture', image, 'sh', '-c', script])
    trace = (OUT / 'linux-syscalls.log').read_text()
    version = (OUT / 'linux-direct-version.log').read_text()
    if not version.startswith('systemd ') or '[Inferior 1 (process ' not in trace or 'exited normally]' not in trace:
        raise RuntimeError('systemd version execution did not complete normally')
    names = {int(node.attrib['number']): node.attrib['name']
             for node in ET.parse(OUT / 'syscalls.xml').iter('syscall')}
    events = [(int(nr), int(value)) for nr, value in
              re.findall(r'^RDXNU_SYSCALL (\d+) (\d+)$', trace, re.M)]
    calls = []
    index = 0
    while index < len(events):
        nr, arg0 = events[index]
        if nr in (93, 94) and index == len(events) - 1:
            calls.append({'number': nr, 'name': names[nr], 'arg0': arg0, 'returns': False})
            break
        if index + 1 >= len(events) or events[index + 1][0] != nr:
            raise RuntimeError(f'unpaired syscall trace at event {index}')
        value = events[index + 1][1]
        if value >= 1 << 63:
            value -= 1 << 64
        calls.append({'number': nr, 'name': names.get(nr, str(nr)), 'arg0': arg0, 'result': value})
        index += 2
    if not calls or calls[-1].get('returns', True) or calls[-1]['arg0'] != 0:
        raise RuntimeError('missing zero exit syscall')
    # These syscall numbers have backends in the files variant. Their supported
    # flags/forms are bounded: this is not proof that systemd's specific use of
    # each syscall already works. A live version-command gate remains required.
    implemented = {25, 56, 57, 62, 63, 64, 66, 78, 79, 80, 93, 94, 172,
                   174, 175, 176, 177, 215, 222, 226}
    observed = sorted({c['number'] for c in calls})
    report = {
        'state': 'arch_systemd_linux_reference_pass_xnu_pending',
        'image': image,
        'packages': (OUT / 'packages.txt').read_text().splitlines(),
        'command': 'ld-linux-aarch64.so.1 --inhibit-cache --library-path <bundle> systemd --version',
        'version_output': version,
        'files': {str(p.relative_to(OUT / 'rootfs')): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted((OUT / 'rootfs').rglob('*')) if p.is_file()},
        'linux_reference_pass': True,
        'xnu_implementation_snapshot': {
            name: hashlib.sha256((HERE.parent / name).read_bytes()).hexdigest()
            for name in ('arch-loader/linux_exec.h', 'terminal_prepare.py', 'memory/dispatch.inc',
                         'files/dispatch.inc', 'files/stat_copyout.h', 'files_prepare.py')
        },
        'xnu_systemd_version_pass': False,
        'xnu_systemd_pid1_pass': False,
        'xnu_systemd_service_pass': False,
        'syscalls': calls,
        'observed_syscalls': [{'number': nr, 'name': names.get(nr, str(nr)),
                               'has_backend_in_xnu_files_variant': nr in implemented} for nr in observed],
        'unimplemented_observed_syscalls': [names.get(nr, str(nr)) for nr in observed if nr not in implemented],
        'trace_sha256': hashlib.sha256((OUT / 'linux-syscalls.log').read_bytes()).hexdigest(),
        'scope': 'Version command only; not a PID 1 or service-manager requirements trace.',
    }
    (OUT / 'reference.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('state', 'image', 'packages', 'unimplemented_observed_syscalls')}, indent=2))


if __name__ == '__main__':
    main()
