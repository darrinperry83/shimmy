#!/usr/bin/env python3
"""Make an intentionally stopping function checkpoint in an already linked VM KC.

Patch after relocation processing: overwriting ADRP instructions in a raw kernel
before kmutil links it invalidates its split-segment relocation records.
Never changes the input collection or any boot configuration.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import uuid
from stock_collection import load_commands, image_uuid, fileset_entries

HERE = Path(__file__).resolve().parent


def sections(data, header=0):
    for cmd, off, _ in load_commands(data, header):
        if cmd != 0x19:
            continue
        segment = data[off+8:off+24].rstrip(b'\0').decode()
        executable = bool(struct.unpack_from('<I', data, off+60)[0] & 4)
        for i in range(struct.unpack_from('<I', data, off+64)[0]):
            p = off + 72 + i*80
            name = data[p:p+16].rstrip(b'\0').decode()
            address, size, position = struct.unpack_from('<QQI', data, p+32)
            yield segment, name, address, size, position, executable, p


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('collection', type=Path)
    parser.add_argument('symbols', type=Path, help='matching unstripped kernel')
    parser.add_argument('symbol')
    parser.add_argument('output', type=Path)
    parser.add_argument('--offset', type=lambda value: int(value, 0), default=0,
                        help='aligned byte offset inside the named function')
    args = parser.parse_args()
    if args.output.resolve() in (args.collection.resolve(), args.symbols.resolve()):
        raise ValueError('output must be a separate file')
    source = args.collection.read_bytes()
    reference = args.symbols.read_bytes()
    entries = fileset_entries(source)
    kernel_header = next(struct.unpack_from('<Q', source, off+16)[0]
        for cmd, off, size in load_commands(source) if cmd == 0x80000035 and
        source[off+struct.unpack_from('<I', source, off+24)[0]:off+size].split(b'\0', 1)[0] == b'com.apple.kernel')
    if image_uuid(source, kernel_header) != image_uuid(reference, 0):
        raise ValueError('symbol image does not match collection kernel UUID')
    symbols = {}
    for line in subprocess.check_output(['xcrun', 'llvm-nm', '-n', '--defined-only', str(args.symbols)], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                symbols[parts[2]] = int(parts[0], 16)
            except ValueError:
                pass
    function_address = symbols[args.symbol]
    function_end = min(a for a in symbols.values() if a > function_address)
    if args.offset < 0 or args.offset % 4 or function_address + args.offset >= function_end:
        raise ValueError('offset must be aligned and inside the function')
    address = function_address + args.offset
    section = next(s for s in sections(reference) if s[2] <= address < s[2]+s[3] and s[5])
    destination = next(s for s in sections(source, kernel_header) if s[:2] == section[:2] and s[5])
    delta = address-section[2]
    obj = args.output.with_suffix('.checkpoint.o')
    subprocess.run(['xcrun', 'clang', '-arch', 'arm64e', '-DRDXNU_ENTRY_POWEROFF=1', '-c', str(HERE/'entry_checkpoint.S'), '-o', str(obj)], check=True)
    object_data = obj.read_bytes()
    text = next(s for s in sections(object_data) if s[1] == '__text')
    if struct.unpack_from('<I', object_data, text[6]+60)[0]:
        raise ValueError('checkpoint has relocations')
    payload = object_data[text[4]:text[4]+text[3]]
    end = min(a for a in symbols.values() if a > address)
    if not payload or len(payload) > end-address or delta+len(payload) > destination[3]:
        raise ValueError('checkpoint would exceed function/section bounds')
    position = destination[4]+delta
    linked_address = destination[2]+delta
    if position+len(payload) > len(source):
        raise ValueError('checkpoint is not file backed')
    output = bytearray(source)
    output[position:position+len(payload)] = payload
    new_uuid = uuid.uuid5(uuid.NAMESPACE_OID, hashlib.sha256(output).hexdigest())
    for cmd, off, _ in load_commands(source):
        if cmd == 0x1b:
            output[off+8:off+24] = new_uuid.bytes
    # Preserve embedded image identities for relocation metadata. Content hashes
    # and a fresh outer collection UUID identify the diagnostic modification.
    if fileset_entries(bytes(output)) != entries:
        raise ValueError('embedded image identities unexpectedly changed')
    args.output.write_bytes(output)
    result = {'input':str(args.collection.resolve()), 'input_sha256':hashlib.sha256(source).hexdigest(),
        'output':str(args.output.resolve()), 'sha256':hashlib.sha256(output).hexdigest(),
        'collection_uuid':str(new_uuid).upper(), 'symbol':args.symbol, 'symbol_offset':args.offset,
        'source_address':hex(address), 'linked_address':hex(linked_address), 'file_offset':position,
        'checkpoint_bytes':len(payload), 'post_link_patch':True, 'intentional_shutdown':True,
        'observed_in_vm':False, 'entry_count':len(entries)}
    args.output.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__ == '__main__':
    main()
