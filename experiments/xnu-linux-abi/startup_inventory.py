#!/usr/bin/env python3
"""Inventory Apple launchd startup contracts without changing installed services."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib

HERE = Path(__file__).resolve().parent
DISPLAY_LABELS = {'com.apple.WindowServer', 'com.apple.loginwindow'}


def inventory(root):
    records = []
    auxiliary = []
    for directory in ('System/Library/LaunchDaemons', 'Library/LaunchDaemons'):
        for path in sorted((root / directory).glob('*.plist')):
            data = path.read_bytes()
            job = plistlib.loads(data)
            if not isinstance(job, dict):
                raise ValueError(f'invalid plist: {path}')
            if not isinstance(job.get('Label'), str):
                auxiliary.append(dict(path=str(path.relative_to(root)),
                                      sha256=hashlib.sha256(data).hexdigest(),
                                      definition=job, disposition='retain'))
                continue
            records.append(dict(path=str(path.relative_to(root)),
                                sha256=hashlib.sha256(data).hexdigest(),
                                label=job['Label'], definition=job,
                                disposition='replace_with_verified_provider'
                                if job['Label'] in DISPLAY_LABELS else 'retain'))
    display = [r for r in records if r['label'] in DISPLAY_LABELS]
    if {r['label'] for r in display} != DISPLAY_LABELS:
        raise ValueError('both stock display/login jobs must be present')
    # Preserve full dictionaries, including private keys. Endpoint declarations
    # are obligations, not evidence of independent daemons or implementation.
    contracts = []
    labels = {r['label'] for r in records}
    for record in display:
        job = record['definition']
        companion = job.get('_Conclave')
        contracts.append(dict(label=job['Label'], source=record['path'],
                              mach_services=job.get('MachServices', {}),
                              conclave=companion,
                              companion_is_separate_job=companion in labels,
                              preserved_job_properties={k: v for k, v in job.items()
                                                       if k not in ('Program', 'ProgramArguments')},
                              replacement_executable=None,
                              replacement_protocols_verified=False))
    collisions = {label: [r['path'] for r in records if r['label'] == label]
                  for label in labels if sum(r['label'] == label for r in records) > 1}
    return dict(root=str(root), jobs=records, duplicate_labels=collisions,
                effective_job_precedence_verified=False, auxiliary_plists=auxiliary,
                display_contracts=contracts,
                installed_services_changed=False,
                startup_order_inferred=False,
                physical_boot_compatibility_proven=False)


def json_value(value):
    # Retain types that occur in real launchd plists, including Data and Date.
    if isinstance(value, bytes):
        return {'plist_data_hex': value.hex()}
    import datetime
    if isinstance(value, datetime.datetime):
        return {'plist_date': value.isoformat()}
    raise TypeError(type(value).__name__)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('/'),
                        help='mounted reference system root, read only')
    parser.add_argument('--output', type=Path,
                        default=HERE / 'out/startup/inventory.json')
    args = parser.parse_args()
    report = inventory(args.root.resolve())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, default=json_value) + '\n')
    print(f'STARTUP_INVENTORY: {len(report["jobs"])} job definitions; no services changed')
    for c in report['display_contracts']:
        print(f'{c["source"]}: {len(c["mach_services"])} Mach endpoints; companion={c["conclave"]}')


if __name__ == '__main__':
    main()
