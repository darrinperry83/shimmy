#!/usr/bin/env python3
"""Boot the full installer target under real Linux PID 1, without changing XNU."""
import hashlib,json,re,subprocess,uuid
from pathlib import Path
HERE=Path(__file__).resolve().parent;ABI=HERE.parent;OUT=ABI/'out/installer'

def run(args,timeout=180):
    r=subprocess.run(list(map(str,args)),text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)
    if r.returncode:raise RuntimeError(r.stdout)
    return r.stdout

def main():
    latest=json.loads((OUT/'latest.json').read_text());manifest=ABI/latest['manifest']
    profile=json.loads(manifest.read_text());directory=manifest.parent/'reference';directory.mkdir(exist_ok=True)
    archive=ABI/profile['root_archive'];digest=hashlib.sha256()
    with archive.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):digest.update(chunk)
    if digest.hexdigest()!=profile['archive_sha256']:raise RuntimeError('installer archive changed')
    # Boot an import of the actual archive, not just its source image. This
    # catches missing root paths or metadata errors introduced by the capture.
    archive_image=run(['docker','import',archive],timeout=180).strip()
    source=directory/'input';source.mkdir(exist_ok=True)
    (source/'rdxnu-reference.target').write_text('[Unit]\nDescription=Linux reference supervisor\nRequires=rdxnu-installer.target rdxnu-reference-complete.service\nAfter=rdxnu-installer.target rdxnu-reference-complete.service\n')
    (source/'rdxnu-reference-complete.service').write_text('''[Unit]
Description=Capture completed installer boot (Linux reference only)
Requires=rdxnu-installer.target
After=rdxnu-installer.target
SuccessAction=exit-force
SuccessActionExitStatus=0
FailureAction=exit-force
FailureActionExitStatus=23

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/rdxnu-reference-complete
StandardOutput=journal+console
StandardError=journal+console
''')
    (source/'complete.sh').write_text('''#!/usr/bin/bash
set -euo pipefail
[[ $(cat /proc/1/comm) == systemd ]]
systemctl is-active --quiet rdxnu-installer.target
systemctl is-active --quiet rdxnu-installer-ready.service
failed=$(systemctl --failed --plain --no-legend --no-pager)
[[ -z "$failed" ]] || { printf '%s\\n' "$failed" >&2; exit 1; }
systemctl list-units --all --no-pager --output=json > /proof/units.json
systemctl show --property=ActiveState,SubState,ActiveEnterTimestampMonotonic,After,Requires,Wants rdxnu-installer.target > /proof/boot-target.txt
pacman -Q > /proof/packages.txt
printf 'RDXNU_ARCH_FULL_INIT_REFERENCE_PASS\\n'
''')
    name='rdxnu-installer-reference-'+uuid.uuid4().hex[:12]
    install='set -eu\ninstall -Dm644 /input/rdxnu-reference.target /etc/systemd/system/rdxnu-reference.target\ninstall -Dm644 /input/rdxnu-reference-complete.service /etc/systemd/system/rdxnu-reference-complete.service\ninstall -Dm755 /input/complete.sh /usr/local/libexec/rdxnu-reference-complete\n'
    try:
        run(['docker','run','--name',name,'--network','none','-v',f'{source}:/input:ro',archive_image,'sh','-c',install])
        image=run(['docker','commit',name]).strip()
    finally:run(['docker','rm','-f',name])
    name+='-boot';command=['docker','run','--name',name,'--tty','--network','none','--privileged','--cgroupns','private',
        '--tmpfs','/run','--tmpfs','/tmp','-v',f'{directory}:/proof','-e','container=docker',image,
        '/usr/lib/systemd/systemd','--system','--unit=rdxnu-reference.target','--log-target=console','--log-level=info','--log-color=no']
    try:
        try:r=subprocess.run(command,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=150)
        except subprocess.TimeoutExpired as exc:
            output=exc.stdout or b''
            if isinstance(output,bytes):output=output.decode(errors='replace')
            (directory/'boot-raw.log').write_text(output);raise RuntimeError('full init reference timed out; inspect boot-raw.log') from exc
    finally:run(['docker','rm','-f',name])
    (directory/'boot-raw.log').write_text(r.stdout)
    log=re.sub(r'\x1b\][^\x1b]*(?:\x1b\\|\x07)','',r.stdout)
    log=re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','',log).replace('\r','')
    (directory/'boot.log').write_text(log)
    if r.returncode or 'RDXNU_ARCH_INSTALLER_INIT_READY' not in log or 'RDXNU_ARCH_FULL_INIT_REFERENCE_PASS' not in log:
        raise RuntimeError(f'full init reference failed ({r.returncode}); see {directory}/boot.log\n{log[-3000:]}')
    units=json.loads((directory/'units.json').read_text());states={x['unit']:x for x in units}
    required=('sysinit.target','basic.target','multi-user.target','rdxnu-installer.target','console-getty.service',
              'systemd-networkd.service','systemd-resolved.service','rdxnu-installer-ready.service',
              'systemd-journald.service','dbus-broker.service','systemd-logind.service','systemd-udevd.service')
    for unit in required:
        if states.get(unit,{}).get('active')!='active':raise RuntimeError('unit not active: '+unit)
    failed=[u['unit'] for u in units if u['active']=='failed']
    if failed:raise RuntimeError('failed units: '+str(failed))
    if (directory/'packages.txt').read_text()!=(manifest.parent/'packages.txt').read_text():raise RuntimeError('package set changed at reference boot')
    evidence=dict(installer_manifest=str(manifest.relative_to(ABI)),installer_archive_sha256=profile['archive_sha256'],
        installer_image=profile['installer_image'],booted_archive_image=archive_image,linux_full_init_reference_pass=True,manager_exit_status=r.returncode,
        active_units=len([u for u in units if u['active']=='active']),failed_units=failed,
        required_unit_states={u:states[u] for u in required},
        logs={f:hashlib.sha256((directory/f).read_bytes()).hexdigest() for f in ('boot.log','units.json','boot-target.txt','packages.txt')},
        xnu_full_init_boot_pass=False,xnu_installer_pass=False,interactive_console_input_pass=False,
        target_disk_install_pass=False,scope='Full installer target in a real Linux PID 1 container; container device conditions apply. No native XNU init, interactive console or target installation qualification.')
    (directory/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n');print(json.dumps(evidence,indent=2))

if __name__=='__main__':main()
