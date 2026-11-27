#!/usr/bin/env python3
"""Check the pinned installer archive for names a case-insensitive root loses.

Read-only preflight. Unicode folding is conservative, not an APFS collation
implementation; ASCII case pairs are definitive for case-insensitive APFS.
"""
import hashlib
import json
from pathlib import Path
import tarfile
import unicodedata

ABI = Path(__file__).resolve().parent.parent


def inspect_archive(archive, expected_sha256):
    digest = hashlib.sha256()
    with archive.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    if digest.hexdigest() != expected_sha256:
        raise ValueError('installer archive differs from its pinned manifest')
    seen = {}
    collisions = []
    with tarfile.open(archive, 'r|gz') as stream:
        for member in stream:
            name = member.name.removeprefix('./').rstrip('/')
            if name.startswith('/') or '..' in Path(name).parts:
                raise ValueError('unsafe archive path')
            folded = unicodedata.normalize('NFD', name).casefold()
            previous = seen.setdefault(folded, name)
            if previous != name:
                collisions.append(dict(first=previous, second=name,
                                       ascii_case_pair=previous.isascii() and name.isascii()
                                       and previous.lower() == name.lower()))
    return dict(archive_sha256=expected_sha256, collision_candidate_count=len(collisions),
                ascii_case_pair_count=sum(pair['ascii_case_pair'] for pair in collisions),
                collisions=collisions,
                plain_case_insensitive_root_qualified=False,
                scope='Filename preflight only; absence of collisions does not qualify filesystem or permission semantics.')


def main():
    latest = json.loads((ABI / 'out/installer/latest.json').read_text())
    manifest = json.loads((ABI / latest['manifest']).read_text())
    report = inspect_archive(ABI / manifest['root_archive'], manifest['archive_sha256'])
    destination = (ABI / latest['manifest']).parent / 'directory-check.json'
    destination.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
