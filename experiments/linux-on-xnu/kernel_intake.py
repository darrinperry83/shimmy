#!/usr/bin/env python3
"""Pin Linux/Arch source inputs and review updates; never convert/build/install a kernel."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

REQUIRED = ('Makefile', 'arch/arm64/Kconfig', 'kernel/fork.c', 'mm/mmap.c', 'fs/exec.c')
DOMAINS = {
    'architecture': ('arch/arm64/', 'arch/lkl/', 'arch/rdxnu/'),
    'tasks_signals_scheduling': ('kernel/', 'include/linux/sched', 'include/linux/signal'),
    'memory': ('mm/', 'include/linux/mm'),
    'filesystems': ('fs/',),
    'network': ('net/',),
    'userspace_interfaces': ('include/uapi/', 'include/asm-generic/',),
    'device_transport': ('drivers/virtio/', 'drivers/block/', 'drivers/net/'),
}


def git(root, *args):
    result = subprocess.run(['git', '-C', str(root), *args], capture_output=True)
    if result.returncode:
        raise ValueError(result.stderr.decode(errors='replace').strip())
    return result.stdout


def revision(root, ref):
    return git(root, 'rev-parse', '--verify', '--end-of-options', ref + '^{commit}').decode().strip()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def recipe_identity(path):
    """Read simple identity fields without sourcing shell or running PKGBUILD hooks."""
    data = Path(path).read_bytes()
    fields = {}
    for line in data.decode().splitlines():
        match = re.fullmatch(r'(pkgbase|pkgver|pkgrel|_srcname)=([A-Za-z0-9._+\-]+)', line.strip())
        if match:
            fields[match[1]] = match[2]
    return {'sha256': digest(data), 'literal_fields': fields,
            'executed': False, 'source_arrays_or_hooks_evaluated': False}


def config_identity(path):
    data = Path(path).read_bytes()
    options = dict(re.findall(r'^(CONFIG_[A-Za-z0-9_]+)=(.+)$', data.decode(), re.M))
    if options.get('CONFIG_ARM64') != 'y':
        raise ValueError('the baseline Arch configuration must select CONFIG_ARM64=y')
    keys = ('CONFIG_ARM64', 'CONFIG_64BIT', 'CONFIG_SMP', 'CONFIG_MMU',
            'CONFIG_ARM64_4K_PAGES', 'CONFIG_ARM64_16K_PAGES', 'CONFIG_ARM64_64K_PAGES',
            'CONFIG_CGROUPS', 'CONFIG_NAMESPACES', 'CONFIG_DEVTMPFS', 'CONFIG_PROC_FS',
            'CONFIG_SYSFS', 'CONFIG_FUTEX', 'CONFIG_EPOLL')
    return {'sha256': digest(data), 'selected_baseline_options': {k: options[k] for k in keys if k in options},
            'usable_hosted_port_configuration': False}


def inspect(source, ref='HEAD', baseline=None, recipe=None, config=None):
    root = Path(source).resolve()
    top = Path(git(root, 'rev-parse', '--show-toplevel').decode().strip()).resolve()
    if top != root:
        raise ValueError('--source must be the root of the Linux Git checkout')
    commit = revision(root, ref)
    inputs = {}
    for name in REQUIRED:
        data = git(root, 'show', f'{commit}:{name}')
        inputs[name] = digest(data)
    makefile = git(root, 'show', f'{commit}:Makefile').decode()
    fields = dict(re.findall(r'^(VERSION|PATCHLEVEL|SUBLEVEL|EXTRAVERSION)[ \t]*=[ \t]*([^\n]*)$', makefile, re.M))
    if not all(fields.get(k, '').strip().isdigit() for k in ('VERSION', 'PATCHLEVEL', 'SUBLEVEL')):
        raise ValueError('unrecognized Linux Makefile version fields')
    version = '.'.join(fields[k].strip() for k in ('VERSION', 'PATCHLEVEL', 'SUBLEVEL')) + fields.get('EXTRAVERSION', '').strip()
    report = {'schema_version': 1, 'state': 'source_intake_only',
              'source_commit': commit, 'source_tree': git(root, 'rev-parse', f'{commit}^{{tree}}').decode().strip(),
              'linux_makefile_version': version, 'input_file_sha256': inputs,
              'working_tree_used_as_build_input': False,
              'working_tree_dirty': bool(git(root, 'status', '--porcelain', '--untracked-files=no')),
              'arch_recipe': recipe_identity(recipe) if recipe else None,
              'arch_config': config_identity(config) if config else None,
              'baseline_commit': None, 'changed_paths': [], 'review_domains': {},
              'source_port_implemented': False, 'kernel_converted': False,
              'kernel_built': False, 'boot_configuration_changed': False,
              'systemd_on_xnu_proven': False, 'update_compatibility_proven': False,
              'activation_allowed': False,
              'next_gate': 'Implement and validate a hosted Linux architecture port; intake is not compatibility.'}
    recipe_version = (report['arch_recipe'] or {}).get('literal_fields', {}).get('pkgver')
    report['recipe_source_version_match'] = version == recipe_version if recipe_version else None
    report['input_warnings'] = []
    if recipe_version and version != recipe_version:
        report['input_warnings'].append('Arch recipe version differs from the examined Linux source; not a matching build input set')
    if baseline:
        old = revision(root, baseline)
        changed = git(root, 'diff', '--name-only', '-z', old, commit, '--').decode().split('\0')
        changed = [p for p in changed if p]
        report.update(baseline_commit=old, changed_paths=changed,
                      review_domains={name: [p for p in changed if p.startswith(prefixes)]
                                      for name, prefixes in DOMAINS.items()})
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--revision', default='HEAD')
    parser.add_argument('--baseline', help='previous imported source revision for a review report')
    parser.add_argument('--recipe', type=Path, help='Arch PKGBUILD to fingerprint, never execute')
    parser.add_argument('--config', type=Path, help='baseline Arch ARM64 config, not a hosted-port config')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = inspect(args.source, args.revision, args.baseline, args.recipe, args.config)
        # Never replace another checkpoint, and never write into the source input.
        destination = args.output.resolve()
        if destination.is_relative_to(args.source.resolve()):
            raise ValueError('output must be outside the input Linux checkout')
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open('x') as stream:
            json.dump(report, stream, indent=2)
            stream.write('\n')
    except (ValueError, OSError) as error:
        parser.exit(2, f'intake refused: {error}\n')
    print(f'SOURCE_INTAKE_RECORDED: {report["linux_makefile_version"]} {report["source_commit"]}')
    print('NOT_CONVERTED: no hosted kernel build, systemd support or update compatibility is claimed')


if __name__ == '__main__':
    main()
