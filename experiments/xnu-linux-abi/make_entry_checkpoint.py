#!/usr/bin/env python3
"""Create a deliberately halted VMAPPLE entry checkpoint from a copied kernel.

Never edits its source image, host policy, or a boot configuration. The marker
replaces the entry trampoline and verified zero/NOP alignment padding only.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import struct
import subprocess
import uuid

HERE = Path(__file__).resolve().parent

def commands(data):
    offset = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, size = struct.unpack_from('<II', data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError('invalid Mach-O command')
        yield cmd, offset
        offset += size

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--mode', choices=('framebuffer', 'poweroff'), default='framebuffer')
    parser.add_argument('--symbol', help='replace a named function with the poweroff checkpoint')
    parser.add_argument('--symbols', type=Path, help='matching unstripped Mach-O for function addresses')
    args = parser.parse_args()
    if args.kernel.resolve() == args.output.resolve():
        raise ValueError('source and output must differ')
    # Derive layout declarations from the pinned header, then compile assertions.
    header = (HERE/'out/xnu/pexpert/pexpert/arm64/boot.h').read_text()
    video = re.search(r'struct Boot_Video \{.*?\n\};', header, re.S).group(0)
    boot = re.search(r'typedef struct boot_args \{.*?\n\} boot_args;', header, re.S).group(0)
    layout = '#include <stdint.h>\n#include <stddef.h>\n#define BOOT_LINE_LENGTH 1024\n' + video + '\ntypedef struct Boot_Video Boot_Video;\n' + boot
    for field, offset in [('v_baseAddr',40),('v_rowBytes',56),('v_width',64),('v_height',72),('v_depth',80)]:
        layout += f'\n_Static_assert(offsetof(boot_args, Video) + offsetof(Boot_Video, {field}) == {offset}, "boot video offset");'
    subprocess.run(['xcrun','clang','-arch','arm64e','-x','c','-fsyntax-only','-'], input=layout, text=True, check=True)
    source = args.kernel.read_bytes()
    if struct.unpack_from('<I', source)[0] != 0xfeedfacf or b'VMAPPLE' not in source:
        raise ValueError('expected VMAPPLE thin Mach-O kernel')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    obj = args.output.with_suffix('.checkpoint.o')
    compile_command = ['xcrun', 'clang', '-arch', 'arm64e', '-c', str(HERE/'entry_checkpoint.S'), '-o', str(obj)]
    if args.mode == 'poweroff':
        compile_command.append('-DRDXNU_ENTRY_POWEROFF=1')
    subprocess.run(compile_command, check=True)
    data = obj.read_bytes()
    code = None
    for cmd, offset in commands(data):
        if cmd == 0x19:
            for i in range(struct.unpack_from('<I', data, offset+64)[0]):
                sec = offset+72+i*80
                if data[sec:sec+16].rstrip(b'\0') == b'__text':
                    size, pos = struct.unpack_from('<QI', data, sec+40)
                    code = data[pos:pos+size]
                    if struct.unpack_from('<I', data, sec+60)[0]:
                        raise ValueError('checkpoint must have no relocations')
    if not code or len(code) > 256:
        raise ValueError('invalid checkpoint code')
    output = bytearray(source)
    entry = next(struct.unpack_from('<Q', source, off+272)[0] for cmd, off in commands(source) if cmd == 5)
    checkpoint = entry
    if args.symbol:
        if args.mode != 'poweroff':
            raise ValueError('named function checkpoints require poweroff mode')
        symbol_path = args.symbols or args.kernel
        symbols_data = symbol_path.read_bytes()
        get_uuid = lambda data: next(data[off+8:off+24] for cmd, off in commands(data) if cmd == 0x1b)
        if get_uuid(source) != get_uuid(symbols_data):
            raise ValueError('symbol image UUID does not match kernel')
        symbols = {}
        listing = subprocess.check_output(['xcrun', 'llvm-nm', '-n', '--defined-only', str(symbol_path)], text=True)
        for line in listing.splitlines():
            parts = line.split()
            if len(parts) == 3:
                try:
                    address = int(parts[0], 16)
                except ValueError:
                    continue
                symbols[parts[2]] = (address, parts[1])
        checkpoint, kind = symbols[args.symbol]
        # Mach-O __TEXT_EXEC symbols may be classified as 's'; the segment's
        # execute permission below is the authoritative check.
        if kind.lower() not in ('t', 's'):
            raise ValueError('checkpoint symbol is not text')
        end = min(address for address, _ in symbols.values() if address > checkpoint)
        if end - checkpoint < len(code):
            raise ValueError('checkpoint code would cross next symbol')
    mappings = []
    for cmd, off in commands(source):
        if cmd == 0x19:
            va, size, pos, file_size = struct.unpack_from('<QQQQ', source, off+24)
            if va <= checkpoint and checkpoint+len(code) <= va+file_size:
                if not struct.unpack_from('<I', source, off+60)[0] & 4:
                    raise ValueError('checkpoint segment is not executable')
                mappings.append(pos+checkpoint-va)
    if len(mappings) != 1:
        raise ValueError('entry not uniquely backed by file bytes')
    pos = mappings[0]
    for i in range(pos+8, pos+len(code), 4) if not args.symbol else ():
        if source[i:i+4] not in (b'\0'*4, bytes.fromhex('1f2003d5')):
            raise ValueError('entry is not followed by zero/NOP alignment padding')
    output[pos:pos+len(code)] = code
    new_uuid = uuid.uuid5(uuid.NAMESPACE_OID, hashlib.sha256(output).hexdigest())
    for cmd, off in commands(source):
        if cmd == 0x1b:
            output[off+8:off+24] = new_uuid.bytes
    args.output.write_bytes(output)
    report = {'input':str(args.kernel.resolve()),'input_sha256':hashlib.sha256(source).hexdigest(),
        'output':str(args.output.resolve()),'sha256':hashlib.sha256(output).hexdigest(),
        'kernel_uuid':str(new_uuid).upper(),'entry':hex(entry),'file_offset':pos,
        'checkpoint_address':hex(checkpoint),'checkpoint_symbol':args.symbol,
        'checkpoint_bytes':len(code),'intentional_halt':True,'mode':args.mode,
        'expected_signal':'guest stops via PSCI SYSTEM_OFF' if args.mode == 'poweroff' else 'red 128x64 tile at top left',
        'observed_in_vm':False,'boot_args_video_offset':40}
    args.output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__ == '__main__': main()
