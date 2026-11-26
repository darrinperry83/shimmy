#!/usr/bin/env python3
"""Build an isolated VM control or opt-in SVC probe, retaining prepared support.

Uses isolated output directories; never installs or boots an artifact. Restores
sleh.c in a finally block and holds the shared kernel-build lock throughout.
"""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import subprocess
import prepare

HERE = Path(__file__).resolve().parent
OUT = HERE / 'out'
XNU = OUT / 'xnu'
TARGET = OUT / 'vmapple/baseline'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-lto', action='store_true', help='isolate the reference build recipe LTO setting')
    parser.add_argument('--no-bti', action='store_true',
        help='VM diagnostic variant matching platform code without BTI landing pads')
    parser.add_argument('--configuration', choices=('DEVELOPMENT', 'RELEASE'), default='DEVELOPMENT')
    parser.add_argument('--with-svc-probe', action='store_true',
        help='retain the P0 dispatch in an isolated DEVELOPMENT build')
    parser.add_argument('--with-elf', action='store_true', help='isolated DEVELOPMENT M1 ELF experiment')
    parser.add_argument('--with-arch-loader', action='store_true')
    parser.add_argument('--with-linux-console', action='store_true')
    parser.add_argument('--with-linux-terminal', action='store_true')
    parser.add_argument('--with-linux-core', action='store_true', help='isolated opt-in freestanding Linux-core host test')
    parser.add_argument('--with-linux-memory', action='store_true', help='XNU-backed Linux memory personality test')
    parser.add_argument('--with-linux-filemap', action='store_true', help='private file mapping and fixed replacement test')
    parser.add_argument('--with-linux-files', action='store_true', help='native descriptor/path and Linux stat ABI test')
    parser.add_argument('--with-linux-paths', action='store_true', help='native O_PATH descriptors and statx')
    parser.add_argument('--with-linux-process', action='store_true', help='native Linux fork and waitid lifecycle')
    parser.add_argument('--with-linux-startup', action='store_true', help='heap, native TLS preservation and libc startup')
    parser.add_argument('--startup-entry-mode', choices=('kdk-bridge','stock'), default='kdk-bridge',
                        help='startup diagnostic: preserve KDK entry with TLS hooks or use stock calls')
    args = parser.parse_args()
    if args.with_linux_process:
        args.with_linux_paths = True
    if args.with_linux_paths:
        args.with_linux_startup = True
    if args.with_linux_startup:
        args.with_linux_files = True
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
    target = TARGET.with_name('baseline-no-lto') if args.no_lto else TARGET
    if args.configuration == 'RELEASE':
        target = target.with_name(target.name + '-release')
    if args.no_bti:
        target = target.with_name(target.name + '-no-bti')
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
    if args.with_linux_startup:
        target = target.with_name(target.name + '-startup')
        if args.startup_entry_mode != 'patched':
            target = target.with_name(target.name + '-' + args.startup_entry_mode)
    if args.with_linux_paths:
        target = target.with_name(target.name + '-paths')
    if args.with_linux_process:
        target = target.with_name(target.name + '-process')
    target.mkdir(parents=True, exist_ok=True)
    with (OUT / 'kernel-build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        source = XNU / prepare.SLEH
        original = subprocess.check_output(['git', '-C', str(XNU), 'show', f'HEAD:{prepare.SLEH}'])
        saved = source.read_bytes()
        build_source = saved if args.with_svc_probe else original
        if saved != prepare.patched(original).encode():
            raise ValueError('unexpected SVC source; refusing to replace it')
        extra_edits = {}
        if args.with_elf:
            import elf_prepare
            extra_edits = elf_prepare.edits(XNU, arch_loader=args.with_arch_loader)
            if args.with_linux_console:
                import console_prepare
                extra_edits = console_prepare.overlay(XNU, extra_edits)
            if args.with_linux_terminal:
                import terminal_prepare
                extra_edits = terminal_prepare.overlay(XNU, extra_edits)
            if args.with_linux_core:
                import linux_core_prepare
                extra_edits = linux_core_prepare.overlay(XNU, extra_edits)
            if args.with_linux_memory:
                import memory_prepare
                extra_edits = memory_prepare.overlay(XNU, extra_edits)
            if args.with_linux_files:
                import files_prepare
                extra_edits = files_prepare.overlay(XNU, extra_edits)
            if args.with_linux_startup:
                import startup_prepare
                extra_edits = startup_prepare.overlay(XNU, extra_edits, entry_mode=args.startup_entry_mode)
            if args.with_linux_paths:
                import paths_prepare
                extra_edits = paths_prepare.overlay(XNU, extra_edits)
            if args.with_linux_process:
                import process_prepare
                extra_edits = process_prepare.overlay(XNU, extra_edits)
            build_source = build_source.replace(b'if (p && rdxnu_linux_probe(state, p))',
                b'if (p && (rdxnu_linux_dispatch(state, p) || rdxnu_linux_probe(state, p)))')
            build_source = build_source.replace(b'#include \"rdxnu_linux_probe.h\"',
                b'#include \"rdxnu_linux_probe.h\"\nextern bool rdxnu_linux_dispatch(arm_saved_state_t *, struct proc *);')
        bti_header = XNU / 'pexpert/pexpert/arm64/apple_arm64_common.h'
        saved_bti = bti_header.read_bytes()
        changed_bti = saved_bti
        if args.no_bti:
            anchor = b'#define BTI_ENFORCED 1'
            if saved_bti.count(anchor) != 1:
                raise ValueError('unexpected BTI enforcement header')
            changed_bti = saved_bti.replace(anchor, b'#define BTI_ENFORCED 0 /* VM diagnostic only */')
        command = json.loads((OUT / 'vmapple/kernel-build-result.json').read_text())['command']
        if 'MACHINE_CONFIGS=VMAPPLE' not in command or 'KERNEL_CONFIGS=DEVELOPMENT' not in command:
            raise ValueError('expected VMAPPLE development build command')
        command[command.index('KERNEL_CONFIGS=DEVELOPMENT')] = f'KERNEL_CONFIGS={args.configuration}'
        command += [f'OBJROOT={target / "obj"}', f'SYMROOT={target / "sym"}', f'DSTROOT={target / "dst"}']
        if args.no_lto:
            command.append('BUILD_LTO=0')
        if args.no_bti:
            command.append('BTI_BUILD=0')
        if args.with_linux_core:
            core, core_report = linux_core_prepare.verified_core()
            command.append(f'LD_KERNEL_LIBS=-lcc_kext {core}')
        if args.with_linux_startup and args.startup_entry_mode != 'stock':
            kdk = next(value.split('=',1)[1] for value in command if value.startswith('KDKROOT='))
            support, support_report = startup_prepare.support_archive(target,kdk)
            command.append(f'RDXNU_KERNEL_SUPPORT_ARCHIVE={support}')
        report = {'command': command, 'svc_probe_present': args.with_svc_probe,
                  'svc_source_sha256': hashlib.sha256(build_source).hexdigest(),
                  'other_prepared_build_support_retained': True, 'state': 'building',
                  'source_restored': False, 'booted': False, 'lto_disabled': args.no_lto,
                  'configuration': args.configuration, 'bti_disabled': args.no_bti}
        report['bti_enforcement_disabled'] = args.no_bti
        report['elf_experiment'] = args.with_elf
        report['arch_loader_experiment'] = args.with_arch_loader
        report['linux_console_experiment'] = args.with_linux_console
        report['linux_terminal_experiment'] = args.with_linux_terminal
        report['linux_core_experiment'] = args.with_linux_core
        report['linux_memory_experiment'] = args.with_linux_memory
        report['linux_filemap_experiment'] = args.with_linux_filemap
        report['linux_files_experiment'] = args.with_linux_files
        report['linux_startup_experiment'] = args.with_linux_startup
        report['linux_paths_experiment'] = args.with_linux_paths
        report['linux_process_experiment'] = args.with_linux_process
        report['startup_entry_mode'] = args.startup_entry_mode if args.with_linux_startup else None
        if args.with_linux_startup and args.startup_entry_mode != 'stock':
            report['startup_support_archive'] = support_report
        if args.with_linux_core:
            report['linux_core_sha256'] = core_report['core_sha256']
        report['elf_source_sha256'] = {str(path.relative_to(XNU)): hashlib.sha256(after).hexdigest()
                                     for path, (before, after) in extra_edits.items()}
        result = target / 'build-result.json'
        try:
            for path, (before, after) in extra_edits.items():
                path.write_bytes(after)
            source.write_bytes(build_source)
            if args.no_bti:
                bti_header.write_bytes(changed_bti)
            result.write_text(json.dumps(report, indent=2) + '\n')
            with (target / 'build.log').open('w') as log:
                code = subprocess.run(command, cwd=XNU, stdout=log, stderr=subprocess.STDOUT).returncode
            report.update(returncode=code, state='built' if code == 0 else 'failed')
            if code:
                raise RuntimeError(f'baseline build failed ({code}); see {target / "build.log"}')
            if args.with_linux_startup and args.startup_entry_mode != 'stock':
                report['startup_assembly_offsets'] = startup_prepare.verify_offsets(target)
            kernel = target / f'obj/{args.configuration}_ARM64_VMAPPLE/kernel.{args.configuration.lower()}.vmapple'
            data = kernel.read_bytes()
            if (b'rdxnu_linux_probe' in data) != args.with_svc_probe:
                raise ValueError('kernel probe string disagrees with requested variant')
            report.update(kernel=str(kernel), kernel_sha256=hashlib.sha256(data).hexdigest(),
                uuid_output=subprocess.check_output(['xcrun', 'dwarfdump', '--uuid', str(kernel)], text=True).strip())
        finally:
            for path, (before, after) in extra_edits.items():
                if path.read_bytes() != after:
                    raise RuntimeError(f"ELF source changed during build: {path}")
                if before is None:
                    path.unlink()
                else:
                    path.write_bytes(before)
            if bti_header.read_bytes() != changed_bti:
                raise RuntimeError('BTI header changed during build; refusing concurrent overwrite')
            if args.no_bti:
                bti_header.write_bytes(saved_bti)
            if source.read_bytes() != build_source:
                raise RuntimeError('SVC source changed during build; refusing to overwrite concurrent edits')
            source.write_bytes(saved)
            report['source_restored'] = (source.read_bytes() == saved and bti_header.read_bytes() == saved_bti
                and all((not path.exists()) if before is None else path.read_bytes() == before
                        for path, (before, after) in extra_edits.items()))
            result.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
