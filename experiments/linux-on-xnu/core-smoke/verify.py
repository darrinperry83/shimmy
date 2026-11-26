#!/usr/bin/env python3
"""Build/check the pinned native Linux-core prototype. Never installs or boots XNU."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / 'out'
PIN = 'c4d0895ead9badb52f7bbffe3d529c61c8fc9485'
# Case-only Linux header pairs on a case-insensitive checkout; not port edits.
CASE_COLLISIONS = {
    'include/uapi/linux/netfilter/xt_CONNMARK.h',
    'include/uapi/linux/netfilter/xt_DSCP.h',
    'include/uapi/linux/netfilter/xt_MARK.h',
    'include/uapi/linux/netfilter/xt_RATEEST.h',
    'include/uapi/linux/netfilter/xt_TCPMSS.h',
    'include/uapi/linux/netfilter_ipv4/ipt_ECN.h',
    'include/uapi/linux/netfilter_ipv4/ipt_TTL.h',
    'include/uapi/linux/netfilter_ipv6/ip6t_HL.h',
}
MARKERS = ('LINUX_CORE_BOOT_PASS', 'LINUX_CORE_PIPE_EPOLL_PASS', 'LINUX_CORE_VFS_PASS',
           'NATIVE_DARWIN_CONTROL_PASS', 'LINUX_CORE_SMOKE_PASS')


def run(command, **kwargs):
    result = subprocess.run([str(a) for a in command], text=True, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f'{command!r}:\n{result.stdout}{result.stderr}')
    return result.stdout


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=OUT / 'darwin-source')
    parser.add_argument('--clang', default='/opt/homebrew/opt/llvm@19/bin/clang')
    parser.add_argument('--build', action='store_true', help='run the pinned upstream library builder first')
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('this runtime proof requires a native arm64 Darwin host')
    source = args.source.resolve()
    commit = run(['git', '-C', source, 'rev-parse', 'HEAD']).strip()
    if commit != PIN:
        parser.error('unexpected Linux/Darwin source revision')
    dirty = set(run(['git', '-C', source, 'diff', '--name-only', 'HEAD']).splitlines())
    if dirty - CASE_COLLISIONS:
        parser.error('source has modifications beyond the known case-only header collisions')
    compiler = run([args.clang, '--version'])
    if 'clang version 19.' not in compiler or 'Apple clang' in compiler:
        parser.error('the pinned source profile requires upstream LLVM 19')
    OUT.mkdir(exist_ok=True)
    library_dir = OUT / 'darwin-build'
    library = library_dir / 'liblkl.a'
    if args.build:
        print('Building pinned Linux core as native Mach-O; no VM', flush=True)
        command = ['python3', source / 'tools/lkl/darwin/build.py', '--clang', args.clang,
                   '--output', library_dir.resolve(), '--work', (OUT / 'darwin-work').resolve()]
        with (OUT / 'native-core-build.log').open('w') as log:
            subprocess.run([str(a) for a in command], stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=1800)
    if not library.is_file():
        parser.error('library missing; run with --build after preparing the pinned checkout')
    binary = OUT / 'native-core-smoke'
    command = [args.clang, '-Wall', '-Wextra', '-Werror', '-DLKL_HOST_CONFIG_POSIX', '-isystem',
               source / 'tools/lkl/darwin/profile/host-include', HERE / 'smoke.c', library,
               '-o', binary]
    run(command)
    output = run([binary], timeout=40)
    (OUT / 'native-core-smoke.log').write_text(output)
    if any(marker not in output for marker in MARKERS):
        raise RuntimeError('missing runtime success marker')
    report = {
        'state': 'native_linux_core_subsystem_smoke_pass', 'source_commit': commit,
        'source_repository': 'https://github.com/HuanchuanTech/lkl',
        'linux_version': '6.12.0', 'compiler': compiler.splitlines()[0],
        'library_sha256': sha(library), 'library_rebuilt_this_run': args.build,
        'library_provenance_note': 'Without --build, this run tests the existing archive; source pin alone does not attest its build provenance.',
        'binary_sha256': sha(binary), 'test_source_sha256': sha(HERE / 'smoke.c'),
        'binary_format': run(['file', binary]).strip(),
        'runtime_log_sha256': sha(OUT / 'native-core-smoke.log'),
        'passed_markers': list(MARKERS), 'host_backend': 'POSIX host operations in a native Darwin process',
        'virtual_machine_used_for_this_build_or_test': False,
        'custom_xnu_kernel_backend_implemented': False,
        'linux_process_execution_proven': False, 'arch_shell_running': False,
        'systemd_running': False, 'linux_user_kernel_memory_isolation_proven': False,
        'smp_proven': False, 'latest_arch_kernel_supported': False,
        'boot_configuration_changed': False,
    }
    (OUT / 'native-core-smoke-result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
