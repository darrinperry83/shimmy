#!/usr/bin/env python3
"""Link an isolated VM baseline or P0 kernel with the control's release drivers."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from stock_collection import fileset_entries

OUT = Path(__file__).resolve().parent / 'out/vmapple'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-lto', action='store_true')
    parser.add_argument('--no-bti', action='store_true')
    parser.add_argument('--stack-probe-bti', action='store_true',
        help='write a separate VM collection with the verified stack-probe BTI correction')
    parser.add_argument('--configuration', choices=('DEVELOPMENT', 'RELEASE'), default='DEVELOPMENT')
    parser.add_argument('--with-svc-probe', action='store_true')
    parser.add_argument('--with-elf', action='store_true')
    parser.add_argument('--with-arch-loader', action='store_true')
    parser.add_argument('--with-linux-console', action='store_true')
    parser.add_argument('--with-linux-terminal', action='store_true')
    parser.add_argument('--with-linux-core', action='store_true')
    parser.add_argument('--with-linux-memory', action='store_true')
    parser.add_argument('--with-linux-filemap', action='store_true')
    parser.add_argument('--with-linux-files', action='store_true')
    args = parser.parse_args()
    if args.with_linux_files:
        args.with_linux_filemap = True
    if args.with_linux_filemap:
        args.with_linux_memory = True
    if args.with_linux_core and args.with_linux_memory:
        parser.error('the hosted Linux-core and XNU memory experiments are separate variants')
    if args.with_linux_core or args.with_linux_memory:
        args.with_linux_terminal = True
    if args.with_linux_terminal:
        args.with_linux_console = True
    if args.with_linux_console:
        args.with_arch_loader = True
    if args.with_arch_loader:
        args.with_elf = True
    if args.with_elf:
        args.with_svc_probe = True
        if not args.no_bti or not args.no_lto:
            parser.error('--with-elf requires --no-bti --no-lto')
    if args.with_svc_probe and args.configuration != 'DEVELOPMENT':
        parser.error('--with-svc-probe requires DEVELOPMENT')
    target = OUT / ('baseline-no-lto' if args.no_lto else 'baseline')
    if args.configuration == 'RELEASE':
        target = target.with_name(target.name + '-release')
    if args.no_bti:
        target = target.with_name(target.name + '-no-bti')
    if args.no_bti and args.stack_probe_bti:
        raise ValueError('stack-probe BTI correction belongs to the BTI-enabled variant')
    if args.with_svc_probe:
        target = target.with_name(target.name + '-svc-probe')
    if args.with_elf:
        target = target.with_name(target.name + '-elf')
    if args.with_arch_loader:
        target = target.with_name(target.name + '-arch-loader')
    if args.with_linux_console:
        target = target.with_name(target.name + '-linux-console')
    if args.with_linux_terminal:
        target = target.with_name(target.name + '-terminal')
    if args.with_linux_core:
        target = target.with_name(target.name + '-linux-core')
    if args.with_linux_memory:
        target = target.with_name(target.name + '-memory')
    if args.with_linux_filemap:
        target = target.with_name(target.name + '-filemap')
    if args.with_linux_files:
        target = target.with_name(target.name + '-files')
    report = json.loads((target / 'build-result.json').read_text())
    if report.get('linux_core_experiment', False) != args.with_linux_core:
        raise ValueError('Linux core build option mismatch')
    if report.get('linux_memory_experiment', False) != args.with_linux_memory:
        raise ValueError('Linux memory build option mismatch')
    if report.get('linux_filemap_experiment', False) != args.with_linux_filemap:
        raise ValueError('Linux filemap build option mismatch')
    if report.get('linux_files_experiment', False) != args.with_linux_files:
        raise ValueError('Linux files build option mismatch')
    if report['state'] != 'built' or report['svc_probe_present'] != args.with_svc_probe or not report['source_restored']:
        raise ValueError('baseline build did not complete with restored sources')
    kernel = Path(report['kernel'])
    if hashlib.sha256(kernel.read_bytes()).hexdigest() != report['kernel_sha256']:
        raise ValueError('baseline kernel changed')
    command = json.loads((OUT / 'collection-command.json').read_text())
    command[command.index('--variant-suffix') + 1] = 'release'
    command[command.index('--kernel') + 1] = str(kernel)
    collection = target / 'baseline-release-drivers.kc'
    command[command.index('--boot-path') + 1] = str(collection)
    (target / 'collection-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (target / 'collection-link.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    entries = fileset_entries(collection.read_bytes())
    control = fileset_entries((OUT / 'apple-control-boot.kc').read_bytes())
    if {e['identifier'] for e in entries} != {e['identifier'] for e in control}:
        raise ValueError('baseline entry set differs from working control')
    drivers = lambda records: {e['identifier']: e['uuid'] for e in records if e['identifier'] != 'com.apple.kernel'}
    if drivers(entries) != drivers(control):
        raise ValueError('baseline release driver UUIDs differ from working control')
    kernel_uuid = re.search(r'UUID: ([A-F0-9-]+)', report['uuid_output']).group(1)
    if next(e['uuid'] for e in entries if e['identifier'] == 'com.apple.kernel') != kernel_uuid:
        raise ValueError('baseline kernel identity differs from build')
    subprocess.run(['kmutil', 'inspect', '--boot-path', str(collection), '--no-variant-extension', '--show-kernel-uuid-only'], check=True)
    result = {'collection': str(collection), 'sha256': hashlib.sha256(collection.read_bytes()).hexdigest(),
        'kernel_uuid': kernel_uuid, 'kernel_sha256': report['kernel_sha256'], 'svc_probe_present': args.with_svc_probe,
        'driver_variant': 'release', 'exact_control_driver_uuids': True, 'entry_count': len(entries), 'booted': False,
        'lto_disabled': report.get('lto_disabled', False), 'configuration': args.configuration,
        'elf_experiment': report.get('elf_experiment', False),
        'arch_loader_experiment': report.get('arch_loader_experiment', False),
        'linux_console_experiment': report.get('linux_console_experiment', False),
        'linux_terminal_experiment': report.get('linux_terminal_experiment', False),
        'linux_core_experiment': report.get('linux_core_experiment', False),
        'linux_memory_experiment': report.get('linux_memory_experiment', False),
        'linux_filemap_experiment': report.get('linux_filemap_experiment', False),
        'linux_files_experiment': report.get('linux_files_experiment', False),
        'bti_disabled': report.get('bti_disabled', False),
        'bti_enforcement_disabled': report.get('bti_enforcement_disabled', False)}
    (target / 'collection-result.json').write_text(json.dumps(result, indent=2) + '\n')
    if args.stack_probe_bti:
        corrected = target / 'baseline-release-drivers-bti.kc'
        subprocess.run(['python3', str(Path(__file__).with_name('make_direct_stack_probe.py')),
            str(collection), str(kernel.with_name(kernel.name + '.unstripped')),
            str(corrected), '--mode', 'bti'], check=True)
        result['bti_collection'] = str(corrected)
        result['bti_collection_sha256'] = hashlib.sha256(corrected.read_bytes()).hexdigest()
        result['bti_full_boot_proven'] = False
        (target / 'collection-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    main()
