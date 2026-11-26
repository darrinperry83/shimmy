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


def stage(name, candidate, core=False, memory=False, filemap=False, files=False, startup=False, paths=False):
    report = json.loads((candidate / 'collection-result.json').read_text())
    build = json.loads((candidate / 'build-result.json').read_text())
    data = Path(report['collection']).read_bytes()
    if hashlib.sha256(data).hexdigest() != report['sha256']:
        raise ValueError('collection changed')
    if not build['source_restored'] or report.get('linux_core_experiment', False) != core:
        raise ValueError('unexpected kernel build profile')
    if report.get('linux_memory_experiment', False) != memory:
        raise ValueError('unexpected Linux memory build profile')
    if report.get('linux_filemap_experiment', False) != filemap:
        raise ValueError('unexpected Linux filemap build profile')
    if report.get('linux_files_experiment', False) != files:
        raise ValueError('unexpected Linux files build profile')
    if report.get('linux_startup_experiment', False) != startup:
        raise ValueError('unexpected Linux startup build profile')
    if report.get('linux_paths_experiment',False) != paths:
        raise ValueError('unexpected Linux paths build profile')
    if not report['exact_control_driver_uuids'] or not report['bti_enforcement_disabled'] or report['configuration'] != 'DEVELOPMENT':
        raise ValueError('expected diagnostic VM driver-preserving candidate')
    if startup:
        # macOS virtiofs may cache overwritten host files in Recovery. Give
        # each kernel, fixture, and staging recipe a distinct host filename.
        material=Path(__file__).read_bytes()
        for filename in ('startup-probe','startup-run'):
            material+=(ABI/'out/startup'/filename).read_bytes()
        if paths:
            for filename in ('paths-probe','paths-run'):
                material+=(ABI/'out/paths'/filename).read_bytes()
        token=hashlib.sha256(material).hexdigest()[:8]
        name+=f'-{report["kernel_uuid"][:8].lower()}-{token}'
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
    if filemap:
        verification = json.loads((ABI / 'out/memory/filemap-verification.json').read_text())
        if 'XNU_FILEMAP_PERSONALITY_PASS' not in verification.get('linux_reference',''):
            raise ValueError('Linux filemap reference must pass before staging')
        elf = ABI / 'out/memory/filemap-probe'
        if hashlib.sha256(elf.read_bytes()).hexdigest() != verification['linux_elf_sha256']:
            raise ValueError('Linux filemap fixture changed')
        for filename in ('filemap-probe','filemap-run'):
            artifact = ABI / 'out/memory' / filename
            digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
            shutil.copy2(artifact,results / filename)
            extra_files += f'[ "$("$directory/sha256" "$share/{filename}")" = {digest} ] || exit 1\n'
            extra_files += f'cp "$share/{filename}" "$data/Library/RDXNU-P0/{filename}"\nchmod 755 "$data/Library/RDXNU-P0/{filename}"\nchown root:wheel "$data/Library/RDXNU-P0/{filename}"\n'
        boot = boot.replace('"$base/display-output" "$base/memory-run" "$base/memory-probe"',
                            '"$base/filemap-run" "$base/filemap-probe"\n' +
                            '"$base/display-output" "$base/filemap-run" "$base/filemap-probe"')
    if files:
        verification = json.loads((ABI / 'out/files/verification.json').read_text())
        if 'XNU_FILES_PERSONALITY_PASS' not in verification.get('linux_reference',''):
            raise ValueError('Linux files reference must pass before staging')
        elf = ABI / 'out/files/files-probe'
        if hashlib.sha256(elf.read_bytes()).hexdigest() != verification['linux_elf_sha256']:
            raise ValueError('Linux files fixture changed')
        for filename in ('files-probe','files-run'):
            artifact = ABI / 'out/files' / filename
            digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
            shutil.copy2(artifact,results / filename)
            extra_files += f'[ "$("$directory/sha256" "$share/{filename}")" = {digest} ] || exit 1\n'
            extra_files += f'cp "$share/{filename}" "$data/Library/RDXNU-P0/{filename}"\nchmod 755 "$data/Library/RDXNU-P0/{filename}"\nchown root:wheel "$data/Library/RDXNU-P0/{filename}"\n'
        boot = boot.replace('"$base/display-output" "$base/filemap-run" "$base/filemap-probe"',
                            '"$base/files-run" "$base/files-probe"\n' +
                            '"$base/display-output" "$base/files-run" "$base/files-probe"')
    if startup:
        verification=json.loads((ABI/'out/startup/verification.json').read_text())
        if 'XNU_STARTUP_PERSONALITY_PASS' not in verification.get('linux_reference',''):
            raise ValueError('Linux startup reference must pass before staging')
        if hashlib.sha256((ABI/'out/startup/startup-probe').read_bytes()).hexdigest()!=verification['linux_elf_sha256']:
            raise ValueError('Linux startup fixture changed')
        for filename in ('startup-probe','startup-run'):
            artifact=ABI/'out/startup'/filename
            digest=hashlib.sha256(artifact.read_bytes()).hexdigest()
            staged_name=f'{name}-{filename}'
            shutil.copy2(artifact,results/staged_name)
            extra_files+=f'[ "$("$directory/sha256" "$share/{staged_name}")" = {digest} ] || exit 1\n'
            extra_files+=f'cp "$share/{staged_name}" "$data/Library/RDXNU-P0/{filename}"\nchmod 755 "$data/Library/RDXNU-P0/{filename}"\nchown root:wheel "$data/Library/RDXNU-P0/{filename}"\n'
        reference=json.loads((ABI/'out/systemd/reference.json').read_text())
        if not reference['linux_reference_pass']:raise ValueError('systemd Linux reference missing')
        for relative,digest in reference['files'].items():
            artifact=ABI/'out/systemd/rootfs'/relative
            if hashlib.sha256(artifact.read_bytes()).hexdigest()!=digest:raise ValueError(f'systemd artifact changed: {relative}')
            staged=results/'systemd'/relative;staged.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(artifact,staged)
            extra_files+=f'[ "$("$directory/sha256" "$share/systemd/{relative}")" = {digest} ] || exit 1\n'
            extra_files+=f'mkdir -p "$data/Library/RDXNU-P0/systemd/{Path(relative).parent}"\ncp "$share/systemd/{relative}" "$data/Library/RDXNU-P0/systemd/{relative}"\nchmod 755 "$data/Library/RDXNU-P0/systemd/{relative}"\n'
        (results/'systemd-expected-version.txt').write_text(reference['version_output'])
        extra_files+='cp "$share/systemd-expected-version.txt" "$data/Library/RDXNU-P0/systemd-expected-version.txt"\n'
        systemd_test='''"$base/startup-run" "$base/startup-probe"
systemdlog=/var/tmp/rdxnu-systemd-version.log
systemderr=/var/tmp/rdxnu-systemd-version-stderr.log
if "$base/run-arch" "$base/systemd/usr/lib/ld-linux-aarch64.so.1" --inhibit-cache --library-path "$base/systemd/usr/lib/systemd:$base/systemd/usr/lib" "$base/systemd/usr/lib/systemd/systemd" --version > "$systemdlog" 2> "$systemderr"; then
    head -n 2 "$systemdlog" > /var/tmp/rdxnu-systemd-version-only.log
    if cmp -s /var/tmp/rdxnu-systemd-version-only.log "$base/systemd-expected-version.txt"; then
        echo XNU_SYSTEMD_VERSION_PASS
    else
        echo XNU_SYSTEMD_VERSION_OUTPUT_MISMATCH
    fi
else
    echo XNU_SYSTEMD_VERSION_NOT_YET
fi
cat "$systemdlog"
cat "$systemderr"
"$base/display-output" /bin/cat "$systemdlog"'''
        boot=boot.replace('"$base/display-output" "$base/files-run" "$base/files-probe"',systemd_test)
        boot=boot.replace('    if [ -d "$shared" ]; then',
            '    if [ -d "$shared" ]; then\n        cp /var/tmp/rdxnu-systemd-version.log "$shared/systemd-version.log" 2>/dev/null || true\n        cp /var/tmp/rdxnu-systemd-version-stderr.log "$shared/systemd-version-stderr.log" 2>/dev/null || true')
    if paths:
        verification=json.loads((ABI/'out/paths/verification.json').read_text())
        if 'XNU_PATHS_PERSONALITY_PASS' not in verification.get('linux_reference',''):
            raise ValueError('Linux paths reference must pass before staging')
        if hashlib.sha256((ABI/'out/paths/paths-probe').read_bytes()).hexdigest()!=verification['linux_elf_sha256']:
            raise ValueError('Linux paths fixture changed')
        for filename in ('paths-probe','paths-run'):
            artifact=ABI/'out/paths'/filename
            digest=hashlib.sha256(artifact.read_bytes()).hexdigest()
            staged_name=f'{name}-{filename}'
            shutil.copy2(artifact,results/staged_name)
            extra_files+=f'[ "$("$directory/sha256" "$share/{staged_name}")" = {digest} ] || exit 1\n'
            extra_files+=f'cp "$share/{staged_name}" "$data/Library/RDXNU-P0/{filename}"\nchmod 755 "$data/Library/RDXNU-P0/{filename}"\nchown root:wheel "$data/Library/RDXNU-P0/{filename}"\n'
        boot=boot.replace('"$base/startup-run" "$base/startup-probe"',
            '"$base/startup-run" "$base/startup-probe"\n"$base/paths-run" "$base/paths-probe"')
    if startup:
        # A leftover directory is not proof the host share is mounted.
        old_mount='''    shared='/Volumes/My Shared Files/results'
    if [ ! -d "$shared" ]; then
        mkdir -p /var/tmp/rdxnu-results
        /sbin/mount_virtiofs com.apple.virtio-fs.automount /var/tmp/rdxnu-results || true
        shared=/var/tmp/rdxnu-results/results
    fi'''
        new_mount=r'''    shared_root=$(/sbin/mount | /usr/bin/sed -n 's/^.* on \(.*\) (AppleVirtIOFS.*$/\1/p' | /usr/bin/head -n 1)
    if [ -z "$shared_root" ]; then
        mkdir -p /private/var/tmp/rdxnu-results
        /sbin/mount_virtiofs com.apple.virtio-fs.automount /private/var/tmp/rdxnu-results || return 0
        shared_root=/private/var/tmp/rdxnu-results
    fi
    shared="$shared_root/results"
    [ -d "$shared" ] || shared="$shared_root"'''
        if boot.count(old_mount)!=1:raise ValueError('startup share mount anchor changed')
        boot=boot.replace(old_mount,new_mount)
        # Export identity and each finished gate even if a later kernel call hangs.
        boot=boot.replace('trap export_logs EXIT\n', 'trap export_logs EXIT\n: > /var/tmp/rdxnu-systemd-version.log\n: > /var/tmp/rdxnu-systemd-version-stderr.log\n')
        boot=boot.replace('sysctl kern.uuid kern.kernelcacheuuid kern.bootargs\n',
                          'sysctl kern.uuid kern.kernelcacheuuid kern.bootargs\nexport_logs\n')
        gates=('"$base/probe" --expect-kernel',
               '"$base/elf-run-probe" "$base/elf-probe"',
               '"$base/run-arch" "$base/arch-ld.so" --help',
               '"$base/run-pty" "$base/read-probe"',
               '"$base/memory-run" "$base/memory-probe"',
               '"$base/filemap-run" "$base/filemap-probe"',
               '"$base/files-run" "$base/files-probe"',
               '"$base/startup-run" "$base/startup-probe"')
        if paths:gates+=('"$base/paths-run" "$base/paths-probe"',)
        for gate in gates:
            if boot.count(gate)!=1:raise ValueError(f'missing/duplicate startup log gate: {gate}')
            boot=boot.replace(gate,gate+'\nexport_logs')
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
    elif kind == 'filemap':
        candidate = stage('filemap', OUT / (BASELINE + '-memory-filemap'), memory=True, filemap=True)
    elif kind == 'files':
        candidate = stage('files', OUT / (BASELINE + '-memory-filemap-files'), memory=True, filemap=True, files=True)
    elif kind == 'paths':
        candidate = stage('paths', OUT / (BASELINE + '-memory-filemap-files-startup-kdk-bridge-paths'), memory=True, filemap=True, files=True, startup=True, paths=True)
    elif kind == 'startup':
        candidate = stage('startup', OUT / (BASELINE + '-memory-filemap-files-startup'), memory=True, filemap=True, files=True, startup=True)
    elif kind in ('startup-stock','startup-source-control','startup-kdk-bridge'):
        suffix=kind.removeprefix('startup-')
        candidate = stage(kind, OUT / (BASELINE + '-memory-filemap-files-startup-' + suffix), memory=True, filemap=True, files=True, startup=True)
    else:
        raise ValueError('unknown VM test variant')
    print(json.dumps({'rollback': rollback, 'candidate': candidate}, indent=2))


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind',choices=('core','memory','filemap','files','paths','startup','startup-stock','startup-source-control','startup-kdk-bridge'))
    main(parser.parse_args().kind)
