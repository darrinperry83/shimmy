#!/usr/bin/env python3
"""Build native-domain diagnostics and pin the unchanged ELF's Linux reference."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid

HERE=Path(__file__).resolve().parent
OUT=HERE.parent/'out/init'
BASE='sha256:0151e70bd01e7e4c891e903edf1dacba81656f04c6150bfce78d5a570e4df443'


def run(args,check=True):
    result=subprocess.run(list(map(str,args)),text=True,stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT,timeout=90)
    if check and result.returncode:raise RuntimeError(result.stdout)
    return result


def reference(ordinary=False):
    name='rdxnu-domain-reference-'+uuid.uuid4().hex[:12]
    command=['/capture/domain-probe','member','2147483647','linux','/capture/init-probe']
    if ordinary:command=['sh','-c','/capture/domain-probe member 2147483647 linux /capture/init-probe; result=$?; exit "$result"']
    try:
        result=run(['docker','run','--name',name,'--network','none','--read-only',
                    '--cap-drop','ALL','--pids-limit','64','-v',f'{OUT}:/capture:ro',BASE,*command],False)
        state=json.loads(run(['docker','inspect',name]).stdout)[0]['State']
        marker='DOMAIN_CHECK root>0' if ordinary else 'LINUX_DOMAIN_LIFECYCLE_PASS\n'
        if result.returncode!=(170 if ordinary else 0) or marker not in result.stdout or state['Running'] or state['OOMKilled']:
            raise RuntimeError(f'ordinary={ordinary} exit={result.returncode}\n{result.stdout}')
        if not ordinary and result.stdout!='LINUX_SUBREAPER_PASS\nLINUX_DOMAIN_LIFECYCLE_PASS\n':
            raise RuntimeError('nested subreaper output differs')
        return dict(ordinary_process=ordinary,exit_status=result.returncode,
                    output=result.stdout,container_stopped=True,oom_killed=False)
    finally:run(['docker','rm','-f',name],False)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux-reference',action='store_true')
    args=parser.parse_args();OUT.mkdir(exist_ok=True)
    subreaper=json.loads((OUT/'verification.json').read_text())
    subreaper_digest=hashlib.sha256((OUT/'init-probe').read_bytes()).hexdigest()
    if subreaper_digest!=subreaper['linux_elf_sha256']:raise ValueError('subreaper fixture changed')
    run(['xcrun','clang','--target=aarch64-linux-gnu','-ffreestanding','-fno-builtin',
         '-fno-stack-protector','-ffixed-x18','-O2','-Wall','-Wextra','-Werror',
         '-c',HERE/'domain_probe.c','-o',OUT/'domain-probe.o'])
    run(['ld.lld','-m','aarch64elf','-z','max-page-size=16384','--build-id=none',
         '-static','-e','_start',OUT/'domain-probe.o','-o',OUT/'domain-probe'])
    run(['xcrun','clang','-O2','-Wall','-Wextra','-Werror',HERE/'domain_run.c','-o',OUT/'domain-run'])
    report=dict(linux_elf_sha256=hashlib.sha256((OUT/'domain-probe').read_bytes()).hexdigest(),
                subreaper_elf_sha256=subreaper_digest,
                native_supervisor_sha256=hashlib.sha256((OUT/'domain-run').read_bytes()).hexdigest(),
                xnu_domain_membership_pass=False,xnu_init_domain_pass=False,xnu_full_init_boot_pass=False,
                scope='Linux reference tests lifecycle with PID 1 and an unavailable PID target. Live cross-domain target rejection, privileged admission, reference lifetime and native restart require the XNU supervisor gate. XNU uses native IDs at this checkpoint.')
    if args.linux_reference:report.update(linux_image=BASE,reference=[reference(),reference(True),reference()])
    (OUT/'domain-verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
