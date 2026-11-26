#!/usr/bin/env python3
"""Namespace Mach-O object symbols without changing code or relocation indices.

Only MH_OBJECT little-endian 64-bit files with a final string table are accepted.
N_INDR alias targets are strings too and must be rewritten. Linker-synthesized
section boundary names and the explicit host imports are preserved.
"""
import argparse
from pathlib import Path
import struct

HOST_IMPORTS = {b'_bzero'}


def rewrite(data, prefix=b'_rdl'):
    out = bytearray(data)
    if len(out) < 32:
        raise ValueError('short Mach-O header')
    magic, cpu, subtype, kind, count, size, flags, reserved = struct.unpack_from('<8I', out)
    if magic != 0xfeedfacf or kind != 1 or cpu != 0x100000c:
        raise ValueError('expected little-endian ARM64 MH_OBJECT')
    if 32 + size > len(out):
        raise ValueError('load commands exceed file')
    offset = 32
    symtab = None
    for _ in range(count):
        if offset + 8 > 32 + size:
            raise ValueError('truncated load command')
        command, length = struct.unpack_from('<II', out, offset)
        if length < 8 or offset + length > 32 + size:
            raise ValueError('invalid load command')
        if command == 2:
            if symtab is not None or length != 24:
                raise ValueError('unexpected LC_SYMTAB')
            symtab = offset
        offset += length
    if offset != 32 + size or symtab is None:
        raise ValueError('invalid command table or missing symbols')
    _, _, symbols, number, strings, string_size = struct.unpack_from('<6I', out, symtab)
    if symbols + number * 16 > strings or strings + string_size != len(out):
        raise ValueError('require bounded symbols and final string table')
    old_strings = bytes(out[strings:])
    new_strings = bytearray(old_strings)
    renamed = {}

    def rename(index):
        if not index:
            return index
        if index >= len(old_strings):
            raise ValueError('symbol string offset outside table')
        end = old_strings.find(b'\0', index)
        if end < 0:
            raise ValueError('unterminated symbol')
        name = old_strings[index:end]
        if name.startswith(b'section$') or name in HOST_IMPORTS:
            return index
        if name not in renamed:
            value = prefix + name
            renamed[name] = len(new_strings)
            new_strings.extend(value + b'\0')
        return renamed[name]

    for i in range(number):
        entry = symbols + i * 16
        index, type_, section, desc, value = struct.unpack_from('<IBBHQ', out, entry)
        if type_ & 0xe0:  # Debug/STAB entries are not linkage symbols.
            continue
        struct.pack_into('<I', out, entry, rename(index))
        if type_ & 0x0e == 0x0a:  # N_INDR: value is a string-table alias target.
            struct.pack_into('<Q', out, entry + 8, rename(value))
    struct.pack_into('<I', out, symtab + 20, len(new_strings))
    out[strings:] = new_strings
    return bytes(out), len(renamed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    data, count = rewrite(args.input.read_bytes())
    with args.output.open('xb') as stream:
        stream.write(data)
    print(f'NAMESPACED_SYMBOLS={count}')


if __name__ == '__main__':
    main()
