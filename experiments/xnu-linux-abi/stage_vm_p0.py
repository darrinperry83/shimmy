#!/usr/bin/env python3
"""Stage a built VM P0 and its launchd job locally; never install or boot it."""
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
from stock_collection import image_uuid, fileset_entries

HERE = Path(__file__).resolve().parent
OUT = HERE / 'out/vmapple'


def main():
    target = OUT / 'baseline-no-lto-no-bti-svc-probe'
    report = json.loads((target / 'collection-result.json').read_text())
    build = json.loads((target / 'build-result.json').read_text())
    if not (report['svc_probe_present'] and report['configuration'] == 'DEVELOPMENT'
            and report['bti_enforcement_disabled'] and build['source_restored']):
        raise ValueError('expected restored DEVELOPMENT VM P0 diagnostic build')
    collection = Path(report['collection'])
    data = collection.read_bytes()
    if hashlib.sha256(data).hexdigest() != report['sha256']:
        raise ValueError('collection changed')
    uuid = report['kernel_uuid']
    if next(e['uuid'] for e in fileset_entries(data) if e['identifier'] == 'com.apple.kernel') != uuid:
        raise ValueError('embedded kernel identity mismatch')
    results = OUT / 'results'
    results.mkdir(exist_ok=True)
    subprocess.run(['xcrun', 'clang', '-arch', 'arm64', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(HERE / 'probe.c'), str(HERE / 'probe_syscall.S'),
                    '-o', str(results / 'p0-probe')], check=True)
    shutil.copy2(collection, results / 'p0.kc')
    for source, dest in [('guest_p0_boot.sh', 'guest-p0-boot.sh'),
                         ('guest_p0_setup.sh', 'guest-p0-setup.sh')]:
        shutil.copy2(HERE / source, results / dest)
    job = dict(Label='org.rdxnu.p0',
               ProgramArguments=['/bin/sh', '/Library/RDXNU-P0/boot.sh', uuid],
               RunAtLoad=True, StandardOutPath='/var/tmp/rdxnu-p0-launchd.log',
               StandardErrorPath='/var/tmp/rdxnu-p0-launchd.log')
    (results / 'p0-job.plist').write_bytes(plistlib.dumps(job))
    staged = {**report, 'collection_uuid': image_uuid(data, 0),
              'installed_by_this_script': False,
              'probe_sha256': hashlib.sha256((results / 'p0-probe').read_bytes()).hexdigest()}
    (results / 'p0-staged.json').write_text(json.dumps(staged, indent=2) + '\n')
    print(f'VM_P0_STAGED: {uuid}; local files only')


if __name__ == '__main__':
    main()
