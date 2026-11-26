#!/usr/bin/env python3
"""Build only the pinned Linux core + linker glue for the XNU kernel experiment."""
import concurrent.futures
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import namespace_macho
import trace_source

HERE = Path(__file__).resolve().parent
BASE = HERE.parent
SOURCE = BASE / 'out/darwin-source'
PROFILE = SOURCE / 'tools/lkl/darwin/profile'
OUT = BASE / 'out/xnu-core'
PIN = 'c4d0895ead9badb52f7bbffe3d529c61c8fc9485'
CASE_COLLISIONS = {
    'include/uapi/linux/netfilter/' + name for name in
    ('xt_CONNMARK.h', 'xt_DSCP.h', 'xt_MARK.h', 'xt_RATEEST.h', 'xt_TCPMSS.h')
} | {'include/uapi/linux/netfilter_ipv4/ipt_ECN.h',
     'include/uapi/linux/netfilter_ipv4/ipt_TTL.h',
     'include/uapi/linux/netfilter_ipv6/ip6t_HL.h'}


def run(command):
    return subprocess.check_output([str(x) for x in command], text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace-startup', action='store_true', help='instrument the pinned Linux RAM bootstrap in generated source only')
    args = parser.parse_args()
    if run(['git', '-C', SOURCE, 'rev-parse', 'HEAD']).strip() != PIN:
        raise ValueError('unexpected Linux source pin')
    dirty = set(run(['git', '-C', SOURCE, 'diff', '--name-only', 'HEAD']).splitlines())
    if dirty - CASE_COLLISIONS:
        raise ValueError('unexpected Linux source modifications')
    OUT.mkdir(exist_ok=True)
    # A failed rebuild must not leave a previous successful receipt usable.
    (OUT / 'build-result.json').unlink(missing_ok=True)
    clang = run(['xcrun', '--find', 'clang']).strip()
    members = json.loads((PROFILE / 'objects.json').read_text())
    traced = OUT / 'trace-source/bootmem.c'
    if args.trace_startup:
        traced.parent.mkdir(exist_ok=True)
        traced.write_text(trace_source.instrument((SOURCE / 'arch/lkl/mm/bootmem.c').read_text()))
    common = [clang, '-arch', 'arm64e', '-mkernel', '-mmacosx-version-min=26.5',
              '-ffreestanding', '-nostdinc', '-fno-builtin', '-fno-common', '-fno-strict-aliasing',
              '-fno-stack-protector', '-fno-stack-check', '-mno-red-zone', '-mno-implicit-float',
              '-mno-outline-atomics', '-mbranch-protection=none', '-O2', '-w', '-std=gnu11',
              '-fshort-wchar', '-funsigned-char', '-D__DISABLE_EXPORTS',
              '-I' + str(PROFILE / 'include'), '-I' + str(PROFILE / 'lib'),
              '-I' + str(SOURCE / 'lib/raid6')]
    def compile_one(member):
        output = OUT / member['object']
        output.parent.mkdir(parents=True, exist_ok=True)
        flags = []
        for flag in member['flags']:
            if flag.startswith('-I') and 'generated' in flag:
                flag = '-I' + str(PROFILE / flag[2:].removeprefix('./'))
            elif flag.removeprefix('./').startswith('include/generated/'):
                flag = str(PROFILE / flag.removeprefix('./'))
            flags.append(flag)
        unit = PROFILE / member['source']
        if not unit.is_file():
            unit = SOURCE / member['source']
        if args.trace_startup and member['source'] == 'arch/lkl/mm/bootmem.c':
            unit = traced
        command = common + ['-include', str(HERE / 'compiler_compat.h')] + flags + ['-include', str(SOURCE / 'tools/lkl/darwin/macho_section_compat.h'), '-c', str(unit), '-o', str(output)]
        result = subprocess.run(command, cwd=SOURCE, capture_output=True, text=True)
        output.with_suffix('.log').write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f'{member["source"]}: {result.stderr[:1800]}')
    print(f'Compiling {len(members)} arm64e freestanding Linux units', flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        futures = [pool.submit(compile_one, member) for member in members]
        for i, future in enumerate(concurrent.futures.as_completed(futures), 1):
            future.result()
            if i % 100 == 0:
                print(f'KERNEL_CORE {i}/{len(members)}', flush=True)
    glue = OUT / 'boot_glue.o'
    run([clang, '-arch', 'arm64e', '-mkernel', '-c', SOURCE / 'tools/lkl/darwin/boot_glue.S', '-o', glue])
    objects = [OUT / m['object'] for m in members] + [glue]
    listing = OUT / 'objects.txt'
    listing.write_text('\n'.join(map(str, objects)) + '\n')
    combined = OUT / 'linux-core.raw.o'
    run(['xcrun', 'ld', '-r', '-arch', 'arm64e', '-filelist', listing, '-o', combined])
    data, count = namespace_macho.rewrite(combined.read_bytes())
    core = OUT / 'linux-core.o'
    core.write_bytes(data)
    undef = run(['xcrun', 'nm', '-u', core])
    (OUT / 'undefined.txt').write_text(undef)
    imports = [s for s in undef.splitlines() if not s.startswith('section$')]
    if set(imports) != {'_bzero', '_rdl_lkl_bug', '_rdl_lkl_printf'}:
        raise ValueError(f'unexpected Linux core host imports: {imports}')
    result = {'source_commit': PIN, 'core_sha256': hashlib.sha256(data).hexdigest(),
              'compiler': run([clang, '--version']).splitlines()[0], 'object_count': len(members),
              'namespaced_symbol_count': count, 'host_imports': imports,
              'posix_host_or_libsystem_objects_linked': False, 'architecture': 'arm64e',
              'common_compile_flags': common, 'booted_inside_xnu': False}
    inputs = [Path(__file__), HERE / 'compiler_compat.h', HERE / 'namespace_macho.py',
              HERE / 'trace_source.py',
              PROFILE / 'objects.json', SOURCE / 'tools/lkl/darwin/macho_section_compat.h',
              SOURCE / 'tools/lkl/darwin/boot_glue.S']
    result['build_input_sha256'] = {str(p.relative_to(BASE)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    result['known_case_collision_paths'] = sorted(dirty)
    result['memory_bootstrap_tracing'] = args.trace_startup
    if args.trace_startup:
        result['traced_source_sha256'] = hashlib.sha256(traced.read_bytes()).hexdigest()
    (OUT / 'build-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
