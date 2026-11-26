#!/usr/bin/env python3
"""Stage the built M1 VM collection and test files locally; never install them."""
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
from stock_collection import fileset_entries

HERE = Path(__file__).resolve().parent
OUT = HERE / 'out/vmapple'


def main():
    target = OUT / 'baseline-no-lto-no-bti-svc-probe-elf'
    report = json.loads((target / 'collection-result.json').read_text())
    build = json.loads((target / 'build-result.json').read_text())
    if not (report.get('elf_experiment') and report['configuration'] == 'DEVELOPMENT'
            and report['bti_enforcement_disabled'] and build['source_restored']):
        raise ValueError('expected a restored DEVELOPMENT VM ELF build')
    data = Path(report['collection']).read_bytes()
    if hashlib.sha256(data).hexdigest() != report['sha256']:
        raise ValueError('collection changed')
    if next(e['uuid'] for e in fileset_entries(data) if e['identifier'] == 'com.apple.kernel') != report['kernel_uuid']:
        raise ValueError('embedded kernel identity mismatch')
    verification = json.loads((HERE / 'out/elf-verification.json').read_text())
    elf = HERE / 'out/elf-probe'
    if hashlib.sha256(elf.read_bytes()).hexdigest() != verification['elf_sha256']:
        raise ValueError('ELF changed since local verification')
    results = OUT / 'results'
    results.mkdir(exist_ok=True)
    for source, name in [(Path(report['collection']), 'elf.kc'), (elf, 'elf-probe'),
                         (HERE / 'out/elf-run-probe', 'elf-run-probe'),
                         (HERE / 'guest_elf_boot.sh', 'elf-boot.sh')]:
        shutil.copy2(source, results / name)
    job = dict(Label='org.rdxnu.m1', RunAtLoad=True,
               ProgramArguments=['/bin/sh', '/Library/RDXNU-P0/elf-boot.sh', report['kernel_uuid']],
               StandardOutPath='/var/tmp/rdxnu-elf-launchd.log',
               StandardErrorPath='/var/tmp/rdxnu-elf-launchd.log')
    (results / 'elf-job.plist').write_bytes(plistlib.dumps(job))
    manifest = dict(kernel_uuid=report['kernel_uuid'], collection_sha256=report['sha256'],
                    elf_sha256=verification['elf_sha256'], installed_by_this_script=False)
    (results / 'elf-staged.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
