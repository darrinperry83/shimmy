#!/usr/bin/env python3
"""Run unchanged Arch systemd as a real container PID 1, with two child controls.

Docker is a Linux reference only. This never installs or starts a manager on XNU.
The success unit prints a marker; the failure unit exits 1. Systemd must receive
SIGCHLD, identify each child, and return the configured manager exit status.
"""
import hashlib,json,re,subprocess,uuid
from pathlib import Path
HERE=Path(__file__).resolve().parent
OUT=HERE.parent/'out/systemd-manager'
IMAGE='sha256:0151e70bd01e7e4c891e903edf1dacba81656f04c6150bfce78d5a570e4df443'

def run(argv,timeout=60):
    return subprocess.run(list(map(str,argv)),text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)

def main():
    OUT.mkdir(exist_ok=True)
    inspect=run(['docker','image','inspect',IMAGE,'--format','{{.Id}}'])
    if inspect.returncode or inspect.stdout.strip()!=IMAGE:raise RuntimeError('pinned Arch image unavailable')
    results={}
    for label,command,expected in [('success','/usr/bin/printf RDXNU_MANAGER_CHILD_PASS\\\\n',0),('failure','/usr/bin/false',23)]:
        directory=OUT/label;units=directory/'units';units.mkdir(parents=True,exist_ok=True)
        (units/'rdxnu-proof.target').write_text('[Unit]\nDescription=RDXNU lifecycle reference\nDefaultDependencies=no\nRequires=rdxnu-proof.service\nAfter=rdxnu-proof.service\n')
        (units/'rdxnu-proof.service').write_text(f'''[Unit]
Description=Unchanged Arch child lifecycle proof
DefaultDependencies=no
SuccessAction=exit-force
SuccessActionExitStatus=0
FailureAction=exit-force
FailureActionExitStatus=23

[Service]
Type=oneshot
ExecStart={command}
StandardOutput=append:/proof/child.log
StandardError=append:/proof/child-stderr.log
''')
        for name in ('child.log','child-stderr.log'):(directory/name).write_text('')
        name='rdxnu-manager-'+uuid.uuid4().hex[:12]
        args=['docker','run','--name',name,'--rm','--tty','--network','none','--privileged','--cgroupns','private',
              '--tmpfs','/run','--tmpfs','/tmp','-v',f'{directory.resolve()}:/proof',
              '-e','container=docker','-e','SYSTEMD_UNIT_PATH=/proof/units',
              '-e','SYSTEMD_GENERATOR_PATH=/nonexistent',IMAGE,
              '/usr/lib/systemd/systemd','--system','--unit=rdxnu-proof.target','--log-target=console','--log-level=debug','--log-color=no']
        try:result=run(args)
        finally:run(['docker','rm','-f',name],timeout=15)
        (directory/'manager-raw.log').write_text(result.stdout)
        log=re.sub(r'\x1b\][^\x1b]*(?:\x1b\\|\x07)','',result.stdout)
        log=re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','',log).replace('\r','')
        (directory/'manager.log').write_text(log)
        state='0/SUCCESS (success)' if label=='success' else '1/FAILURE'
        match=re.search(r'rdxnu-proof.service: Child (\d+) belongs to rdxnu-proof.service\.',log)
        if result.returncode!=expected or not match or f'Main process exited, code=exited, status={state}' not in log or 'Received SIGCHLD.' not in log or 'Failed to parse' in log:
            raise RuntimeError(f'{label} manager reference failed: exit {result.returncode}\n{log[-4000:]}')
        child=(directory/'child.log').read_text();stderr=(directory/'child-stderr.log').read_text()
        if child!=('RDXNU_MANAGER_CHILD_PASS\n' if label=='success' else '') or stderr:
            raise RuntimeError(f'{label} child output mismatch: {child!r} {stderr!r}')
        results[label]=dict(manager_exit_status=result.returncode,child_pid=int(match[1]),child_exit_status=0 if label=='success' else 1,
                            received_sigchld=True,child_output=child,manager_log_sha256=hashlib.sha256((directory/'manager.log').read_bytes()).hexdigest())
    outside=run(['docker','run','--rm','--network','none',IMAGE,'sh','-c',
                 '/usr/lib/systemd/systemd --user --log-target=console --log-level=debug; result=$?; exit "$result"'])
    expected_guard='Trying to run as user instance, but the system has not been booted with systemd.\n'
    if outside.returncode!=1 or outside.stdout!=expected_guard:
        raise RuntimeError(f'unbooted user-manager control differs: {outside.returncode} {outside.stdout!r}')
    (OUT/'unbooted-user-manager.log').write_text(outside.stdout)
    meta=run(['docker','run','--rm','--network','none',IMAGE,'sh','-c','pacman -Q systemd glibc; sha256sum /usr/lib/systemd/systemd /usr/bin/printf /usr/bin/false'])
    if meta.returncode:raise RuntimeError(meta.stdout)
    report=dict(linux_reference_pass=True,image=IMAGE,packages_and_hashes=meta.stdout,controls=results,unbooted_user_control=dict(exit_status=outside.returncode,message=outside.stdout),
                xnu_manager_runtime_pass=False,scope='Linux PID 1 container reference; no XNU manager or init-namespace claim')
    (OUT/'reference.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
