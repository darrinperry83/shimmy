#!/usr/bin/env python3
"""Build a complete Arch installer root and record its independent package pin.

Only disposable Docker filesystems and ignored output artifacts are mutated.
No host/VM disk is installed and no XNU boot compatibility is inferred.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import io
import tarfile
import uuid

HERE=Path(__file__).resolve().parent
OUT=HERE.parent/'out/installer'
BASE='sha256:0151e70bd01e7e4c891e903edf1dacba81656f04c6150bfce78d5a570e4df443'
FILES={'rdxnu-installer.target':('/etc/systemd/system/rdxnu-installer.target','644'),
       'rdxnu-installer-ready.service':('/etc/systemd/system/rdxnu-installer-ready.service','644'),
       'console-autologin.conf':('/etc/systemd/system/console-getty.service.d/20-installer.conf','644'),
       '20-installer.network':('/etc/systemd/network/20-installer.network','644'),
       'hosts':('/etc/hosts','644'), 'hostname':('/etc/hostname','644'),
       'ready.sh':('/usr/local/libexec/rdxnu-installer-ready','755')}
REQUIRED=('usr/bin/bash','usr/bin/pacman','usr/bin/pacstrap','usr/bin/arch-chroot',
          'usr/bin/genfstab','usr/lib/systemd/systemd','usr/lib/systemd/systemd-executor',
          'usr/lib/ld-linux-aarch64.so.1','usr/lib/libc.so.6',
          'usr/lib/systemd/system/multi-user.target','usr/lib/systemd/system/basic.target',
          'usr/lib/systemd/system/sysinit.target','etc/pacman.conf')

def run(args,timeout=180):
    r=subprocess.run(list(map(str,args)),text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)
    if r.returncode:raise RuntimeError(r.stdout)
    return r.stdout.strip()

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--updated-container',help='Capture a stopped disposable container after a successful full pacman -Syu arch-install-scripts transaction')
    args=parser.parse_args();OUT.mkdir(exist_ok=True)
    if not args.updated_container:parser.error('--updated-container is required; never silently refresh the proven userspace pin')
    state=json.loads(run(['docker','inspect',args.updated_container]))[0]
    if state['State']['Running'] or state['State']['ExitCode']!=0 or state['Image']!=BASE:
        raise RuntimeError('expected successful stopped installer build from the pinned base image')
    overlay_hashes={p:digest(HERE/p) for p in FILES}
    packages_image=run(['docker','commit',args.updated_container])
    name='rdxnu-installer-profile-'+uuid.uuid4().hex[:12]
    install='set -eu\n'
    for source,(dest,mode) in FILES.items():
        if source in ('hosts','hostname'):continue # Docker bind mounts these; archive them below.
        install+=f'install -D -o 0 -g 0 -m {mode} /input/{source} {dest}\n'
    install+='ln -sfn rdxnu-installer.target /etc/systemd/system/default.target\n'
    install+=': > /etc/machine-id\n'
    try:
        run(['docker','run','--name',name,'--network','none','-v',f'{HERE}:/input:ro',packages_image,'sh','-c',install])
        image=run(['docker','commit',name])
    finally:
        run(['docker','rm','-f',name])
    directory=OUT/image.split(':')[1][:16];directory.mkdir(exist_ok=True)
    packages=run(['docker','run','--rm','--network','none',image,'pacman','-Q'])
    (directory/'packages.txt').write_text(packages+'\n')
    for pkg in ('base','filesystem','systemd','bash','pacman','arch-install-scripts'):
        if not any(line.startswith(pkg+' ') for line in packages.splitlines()):raise RuntimeError(f'missing package {pkg}')
    archive=directory/'rootfs.tar.gz';temporary=directory/'rootfs.partial.tar.gz';raw=directory/'rootfs.partial.tar'
    excludes=['dev','proc','sys','run','tmp','root','home','var/log/*',
              'var/cache/pacman/pkg/*','etc/hosts','etc/hostname','etc/resolv.conf']
    command=['docker','run','--rm','--network','none',image,'tar','--sort=name','--numeric-owner',
             '--acls','--xattrs','--one-file-system','-C','/']
    command += ['--exclude=./'+x for x in excludes]
    command += ['-cf','-','.']
    with raw.open('wb') as dest, (directory/'archive-stderr.log').open('wb') as errors:
        r=subprocess.run(command,stdout=dest,stderr=errors,timeout=180)
        if r.returncode:raise RuntimeError('root archive failed; see archive-stderr.log')
    # Runtime mounts and Docker-injected host files are never copied. Append
    # empty mountpoints and deterministic installer-owned host configuration.
    with tarfile.open(raw,'a') as tar:
        for name in ('dev','proc','sys','run','tmp','root','home'):
            entry=tarfile.TarInfo(name);entry.type=tarfile.DIRTYPE
            entry.mode=0o1777 if name=='tmp' else 0o700 if name=='root' else 0o755;tar.addfile(entry)
        for name in ('hosts','hostname'):
            data=(HERE/name).read_bytes();entry=tarfile.TarInfo('etc/'+name);entry.mode=0o644;entry.size=len(data);tar.addfile(entry,io.BytesIO(data))
        entry=tarfile.TarInfo('etc/resolv.conf');entry.type=tarfile.SYMTYPE;entry.linkname='../run/systemd/resolve/stub-resolv.conf';entry.mode=0o777;tar.addfile(entry)
    with raw.open('rb') as src,temporary.open('wb') as dest:
        with gzip.GzipFile(filename='',mode='wb',fileobj=dest,mtime=0,compresslevel=1) as compressed:
            shutil.copyfileobj(src,compressed,1024*1024)
    temporary.replace(archive)
    members={};count=0;package_records=0;unit_count=0
    wanted=set(REQUIRED)|{dest.lstrip('/') for dest,mode in FILES.values()}
    with tarfile.open(archive,'r|gz') as tar:
        for member in tar:
            path=member.name.removeprefix('./');count+=1
            if path.startswith('/') or '..' in Path(path).parts:raise RuntimeError('unsafe archive member')
            if path.startswith('var/lib/pacman/local/') and path.endswith('/desc'):package_records+=1
            if path.startswith('usr/lib/systemd/system/') and member.isfile():unit_count+=1
            if path in wanted:
                if not member.isfile():raise RuntimeError('required regular file missing: '+path)
                members[path]=dict(sha256=hashlib.sha256(tar.extractfile(member).read()).hexdigest(),mode=oct(member.mode),uid=member.uid,gid=member.gid)
    if set(members)!=wanted:raise RuntimeError('required files absent: '+str(wanted-set(members)))
    for source,(dest,mode) in FILES.items():
        if members[dest.lstrip('/')]['sha256']!=overlay_hashes[source] or digest(HERE/source)!=overlay_hashes[source]:
            raise RuntimeError('installer overlay changed during capture: '+source)
    if package_records!=len(packages.splitlines()):raise RuntimeError('package database is incomplete')
    report=dict(base_image=BASE,installer_image=image,packages_image=packages_image,
                root_archive=str(archive.relative_to(HERE.parent)),archive_sha256=digest(archive),
                archive_bytes=archive.stat().st_size,archive_members=count,package_count=package_records,
                systemd_unit_file_count=unit_count,packages_sha256=digest(directory/'packages.txt'),
                required_files=members,overlay_sha256=overlay_hashes,
                package_transaction_command=state['Config']['Cmd'],
                complete_package_root_built=True,linux_full_init_reference_pass=False,
                xnu_full_init_boot_pass=False,xnu_installer_pass=False,
                limits='Package-root artifact only. Native init/adoption, runtime filesystems, complete units, console, networking and target installation require runtime proof.')
    (directory/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    (OUT/'latest.json').write_text(json.dumps(dict(manifest=str((directory/'manifest.json').relative_to(HERE.parent))),indent=2)+'\n')
    raw.unlink()
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
