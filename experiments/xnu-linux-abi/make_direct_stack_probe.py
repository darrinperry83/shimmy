#!/usr/bin/env python3
"""VM-only stack-probe experiments and the verified BTI entry correction.

Direct mode changes one BLRAA after linking. BTI mode replaces a branch to
the next instruction with BTI C, preserving fall-through and the full helper.
Neither mode installs anything or changes the input collection.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import uuid
from make_collection_checkpoint import sections
from stock_collection import load_commands, image_uuid, fileset_entries


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('collection', type=Path)
    ap.add_argument('symbols', type=Path)
    ap.add_argument('output', type=Path)
    ap.add_argument('--offset', type=lambda s: int(s, 0))
    ap.add_argument('--mode', choices=['direct', 'bti'], default='direct')
    args = ap.parse_args()
    assert args.output.resolve() not in (args.collection.resolve(), args.symbols.resolve())
    source = args.collection.read_bytes()
    reference = args.symbols.read_bytes()
    assert b'ARM64_VMAPPLE' in reference
    kh = next(struct.unpack_from('<Q', source, o+16)[0]
        for c, o, n in load_commands(source) if c == 0x80000035 and
        source[o+struct.unpack_from('<I', source, o+24)[0]:o+n].split(b'\0', 1)[0] == b'com.apple.kernel')
    assert image_uuid(source, kh) == image_uuid(reference, 0)
    symbols = {line.split()[2]: int(line.split()[0], 16) for line in
        subprocess.check_output(['xcrun', 'llvm-nm', '-n', '--defined-only', str(args.symbols)], text=True).splitlines()
        if len(line.split()) == 3}

    def locate(address):
        s = next(s for s in sections(reference) if s[2] <= address < s[2]+s[3] and s[5])
        t = next(t for t in sections(source, kh) if t[:2] == s[:2] and t[5])
        delta = address-s[2]
        return t[4]+delta, t[2]+delta

    output = bytearray(source)
    if args.mode == 'direct':
        assert args.offset is not None
        position, address = locate(symbols['_update']+args.offset)
        _, target = locate(symbols['___chkstk_darwin'])
        assert struct.unpack_from('<I', source, position)[0] == 0xd73f0a11
        delta = target-address
        assert delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27)
        struct.pack_into('<I', output, position, 0x94000000 | ((delta//4) & 0x3ffffff))
        modification = 'BLRAA replaced with BL to existing stack-probe helper'
    else:
        position, address = locate(symbols['___chkstk_darwin'])
        _, target = locate(symbols['___chkstk_darwin_probe'])
        assert target == address+4
        assert struct.unpack_from('<I', source, position)[0] == 0x14000001
        struct.pack_into('<I', output, position, 0xd503245f)
        modification = 'replace branch to next instruction with BTI C and fall through to unchanged probe'
    for c, o, _ in load_commands(source):
        if c == 0x1b:
            output[o+8:o+24] = uuid.uuid5(uuid.NAMESPACE_OID, hashlib.sha256(output).hexdigest()).bytes
    assert fileset_entries(bytes(output)) == fileset_entries(source)
    args.output.write_bytes(output)
    report = {'input_sha256': hashlib.sha256(source).hexdigest(),
        'sha256': hashlib.sha256(output).hexdigest(), 'call': hex(address),
        'target': hex(target), 'modification': modification, 'mode': args.mode,
        'helper_probes_preserved': True, 'observed': False}
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
