#!/usr/bin/env python3
"""Stage the opt-in core VM test and a separate known-working rollback bundle.

Does not install, authenticate or change host/guest boot configuration.
"""
import hashlib
import json
from pathlib import Path
import plistlib
import shutil

ABI = Path(__file__).resolve().parent
CORE_HOST = ABI.parent / 'linux-on-xnu/xnu-host'
OUT = ABI / 'out/vmapple'
BASELINE = 'baseline-no-lto-no-bti-svc-probe-elf-arch-loader-linux-console-terminal'


def stage(name, candidate, core=False, memory=False):
    report = json.loads((candidate / 'collection-result.json').read_text())
    build = json.loads((candidate / 'build-result.json').read_text())
    data = Path(report['collection']).read_bytes()
    if hashlib.sha256(data).hexdigest() != report['sha256']:
        raise ValueError('collection changed')
    if not build['source_restored'] or report.get('linux_core_experiment', False) != core:
        raise ValueError('unexpected kernel build profile')
    if report.get('linux_memory_experiment', False) != memory:
        raise ValueError('unexpected Linux memory build profile')
    if not report['exact_control_driver_uuids'] or not report['bti_enforcement_disabled'] or report['configuration'] != 'DEVELOPMENT':
        raise ValueError('expected diagnostic VM driver-preserving candidate')
    results = OUT / 'results'
    results.mkdir(exist_ok=True)
    shutil.copy2(report['collection'], results / f'{name}.kc')
    boot = (ABI / 'terminal/guest_boot.sh').read_text()
    if core:
        anchor = '"$base/display-output"'
        if boot.count(anchor) != 1:
            raise ValueError('unexpected terminal test script')
        boot = boot.replace(anchor, (CORE_HOST / 'guest_test.inc').read_text() + '\n' + anchor)
    extra_files = ''
    if memory:
        verification = json.loads((ABI / 'out/memory/verification.json').read_text())
        if 'XNU_MEMORY_PERSONALITY_PASS' not in verification.get('linux_reference', ''):
            raise ValueError('Linux memory reference must pass before staging')
        elf = ABI / 'out/memory/memory-probe'
        if hashlib.sha256(elf.read_bytes()).hexdigest() != verification['linux_elf_sha256']:
            raise ValueError('Linux memory fixture changed')
        for filename in ('memory-probe', 'memory-run'):
            artifact = ABI / 'out/memory' / filename
            digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
            shutil.copy2(artifact,results / filename)
            extra_files += f'[ "$("$directory/sha256" "$share/{filename}")" = {digest} ] || exit 1\n'
            extra_files += f'cp "$share/{filename}" "$data/Library/RDXNU-P0/{filename}"\nchmod 755 "$data/Library/RDXNU-P0/{filename}"\nchown root:wheel "$data/Library/RDXNU-P0/{filename}"\n'
        anchor = '"$base/display-output" "$base/run-pty" "$base/read-probe"'
        if boot.count(anchor) != 1:
            raise ValueError('unexpected terminal display test')
        boot = boot.replace(anchor, '"$base/memory-run" "$base/memory-probe"\n' +
                            '"$base/display-output" "$base/memory-run" "$base/memory-probe"')
    boot = boot.replace('terminal.log', f'{name}.log').replace('terminal-boot.log', f'{name}-boot.log')
    boot = boot.replace('rdxnu-terminal-display.log', f'rdxnu-{name}-display.log')
    boot = boot.replace('"$shared/terminal-display.log"', f'"$shared/{name}-display.log"')
    (results / f'{name}-boot.sh').write_text(boot)
    job = dict(Label='org.rdxnu.archloader', RunAtLoad=True,
               ProgramArguments=['/bin/sh', f'/Library/RDXNU-P0/{name}-boot.sh', report['kernel_uuid']],
               StandardOutPath=f'/var/tmp/rdxnu-{name}-launchd.log', StandardErrorPath=f'/var/tmp/rdxnu-{name}-launchd.log')
    (results / f'{name}-job.plist').write_bytes(plistlib.dumps(job))
    bootargs = '-v rdxnu_linux_probe=1 rdxnu_linux_elf=1 rdxnu_linux_console=1'
    if core: bootargs += ' rdxnu_linux_core=1 rdxnu_linux_core_trace=1'
    bootargs += ' -unsafe_kernel_text keepsyms=1 debug=0x108 serial=3'
    recovery = '''#!/bin/sh
set -eu
[ "$(sysctl -n kern.hv_vmm_present)" = 1 ] || exit 1
case "$(uname -v)" in *ARM64_VMAPPLE*) ;; *) exit 1 ;; esac
[ "$(id -u)" = 0 ] || exit 1
[ "$1" = configure ] || exit 2
volume='/Volumes/Macintosh HD'
data=/Volumes/Data
share='/Volumes/My Shared Files/results'
[ "$(/usr/libexec/PlistBuddy -c 'Print :ProductBuildVersion' "$volume/System/Library/CoreServices/SystemVersion.plist")" = 25G342 ] || exit 1
[ -f "$data/Library/RDXNU-P0/probe" ] || exit 1
directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
[ "$("$directory/sha256" "$share/@NAME@.kc")" = @SHA@ ] || exit 1
@EXTRAFILES@
cp "$share/@NAME@-boot.sh" "$data/Library/RDXNU-P0/@NAME@-boot.sh"
chmod 755 "$data/Library/RDXNU-P0/@NAME@-boot.sh"
cp "$share/@NAME@-job.plist" "$data/Library/LaunchDaemons/org.rdxnu.archloader.plist"
chmod 644 "$data/Library/LaunchDaemons/org.rdxnu.archloader.plist"
chown root:wheel "$data/Library/RDXNU-P0/@NAME@-boot.sh" "$data/Library/LaunchDaemons/org.rdxnu.archloader.plist"
plutil -lint "$data/Library/LaunchDaemons/org.rdxnu.archloader.plist"
kmutil configure-boot --compress -v "$volume" -c "$share/@NAME@.kc"
nvram '40a0ddd2-77f8-4392-b4a3-1e7304206516:boot-args=@BOOTARGS@'
echo VM_CUSTOM_BOOT_CONFIGURED
'''.replace('@NAME@', name).replace('@SHA@', report['sha256']).replace('@BOOTARGS@', bootargs).replace('@EXTRAFILES@',extra_files)
    (results / f'{name}-recovery.sh').write_text(recovery)
    installer = '''#!/bin/sh
set -eu
[ "$(sysctl -n kern.hv_vmm_present)" = 1 ] || exit 1
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p /tmp/rdxnu
cp /Volumes/RDXNU-VM/sha256 /tmp/rdxnu/
cp "$base/@NAME@-recovery.sh" /tmp/rdxnu/recovery.sh
exec /Volumes/RDXNU-VM/boot-auth "$@"
'''.replace('@NAME@', name)
    (results / f'{name}-install.sh').write_text(installer)
    staged = dict(report, installed_by_this_script=False)
    (results / f'{name}-staged.json').write_text(json.dumps(staged, indent=2) + '\n')
    return {'name': name, 'kernel_uuid': report['kernel_uuid'], 'sha256': report['sha256']}


def main(kind='core'):
    # Verify/stage rollback first. The original working collection stays intact.
    rollback = stage('core-rollback', OUT / BASELINE, False)
    if kind == 'core':
        candidate = stage('core', OUT / (BASELINE + '-linux-core'), core=True)
    elif kind == 'memory':
        candidate = stage('memory', OUT / (BASELINE + '-memory'), memory=True)
    else:
        raise ValueError('unknown VM test variant')
    print(json.dumps({'rollback': rollback, 'candidate': candidate}, indent=2))


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind',choices=('core','memory'))
    main(parser.parse_args().kind)
