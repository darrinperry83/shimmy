#!/usr/bin/env python3
"""Verify a named live VM startup gate against its staged identity and Linux reference."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

HERE=Path(__file__).resolve().parent
ABI=HERE.parent
RESULTS=ABI/'out/vmapple/results'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def verify(name):
    if not re.fullmatch(r'[a-z0-9-]+',name):raise ValueError('invalid staged name')
    staged=json.loads((RESULTS/f'{name}-staged.json').read_text())
    if not staged.get('linux_startup_experiment') or staged.get('startup_entry_mode')!='kdk-bridge':
        raise ValueError('expected KDK bridge startup profile')
    if digest(RESULTS/f'{name}.kc')!=staged['sha256']:raise ValueError('staged collection changed')
    boot_path=RESULTS/f'{name}-boot.log';boot=boot_path.read_text()
    if f"kern.uuid: {staged['kernel_uuid']}\n" not in boot:raise ValueError('runtime kernel mismatch')
    for marker in ('KERNEL_LINUX_ABI_P0_PASS','LINUX_ELF_M1_PASS','ARCH_ELF_COMMAND_PASS',
                   'LINUX_PTY_READ_PASS','WINDOWSERVER_ABSENT_AT_CHECK_PASS',
                   'XNU_MEMORY_PERSONALITY_PASS','XNU_FILEMAP_PERSONALITY_PASS',
                   'XNU_FILES_PERSONALITY_PASS','XNU_STARTUP_PERSONALITY_PASS','XNU_SYSTEMD_VERSION_PASS'):
        if marker not in boot:raise ValueError(f'missing live gate: {marker}')
    if 'LINUX_STARTUP_RESULT status=0 ' not in boot:raise ValueError('startup fixture did not exit zero')
    stdout_path=RESULTS/'systemd-version.log';stderr_path=RESULTS/'systemd-version-stderr.log'
    stdout=stdout_path.read_text();stderr=stderr_path.read_text()
    reference=json.loads((ABI/'out/systemd/reference.json').read_text())
    expected=reference['version_output']
    if not stdout.startswith(expected):raise ValueError('systemd stdout differs from Linux reference')
    match=re.fullmatch(r'ARCH_ELF_RESULT child=([1-9][0-9]*) status=0 parent=([1-9][0-9]*)\nARCH_ELF_COMMAND_PASS\n',stdout[len(expected):])
    if not match:raise ValueError('missing successful native-parent result or unexpected stdout')
    proof=ABI/'out/startup/runtime-proof';proof.mkdir(exist_ok=True)
    evidence={}
    for label,path in (('boot',boot_path),('stdout',stdout_path),('stderr',stderr_path)):
        frozen=proof/f'{name}-{label}.log';shutil.copy2(path,frozen)
        evidence[label]=dict(path=str(frozen.relative_to(ABI)),sha256=digest(frozen))
    return dict(kernel_uuid=staged['kernel_uuid'],collection_sha256=staged['sha256'],
        staged_name=name,startup_runtime_pass=True,systemd_version_runtime_pass=True,
        native_parent=int(match[2]),linux_child=int(match[1]),
        systemd_version=expected.splitlines()[0],stderr_present=bool(stderr),
        stderr=stderr,proof_logs=evidence,
        limits='Single-thread startup and systemd --version only; no PID 1, service-manager, rootfs isolation, or physical-driver qualification')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('staged_name')
    args=parser.parse_args()
    result=verify(args.staged_name)
    (ABI/'out/startup/runtime-verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
