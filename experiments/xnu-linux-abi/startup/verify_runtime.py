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
    paths=staged.get('linux_paths_experiment',False)
    if paths:
        if 'XNU_PATHS_PERSONALITY_PASS' not in boot or 'LINUX_PATHS_RESULT status=0 ' not in boot:
            raise ValueError('path-descriptor fixture did not pass')
    process=staged.get('linux_process_experiment',False)
    if process and ('XNU_PROCESS_PERSONALITY_PASS' not in boot or 'LINUX_PROCESS_RESULT status=0 ' not in boot):
        raise ValueError('process-lifecycle fixture did not pass')
    if process and ('ARCH_CHILD_EXEC_PASS' not in boot or 'NATIVE_WAITID_CONTROL_PASS' not in boot):
        raise ValueError('unchanged Arch child exec marker missing')
    interp=staged.get('linux_interp_experiment',False)
    interp_logs=[]
    if interp:
        for marker in ('XNU_INTERP_NATIVE_PARENT_RESULT=0','ARCH_DIRECT_CHILD_PASS','LINUX_INTERP_PASS','INTERP_ERROR_CONTROLS_PASS','XNU_INTERP_PERSONALITY_PASS'):
            if marker not in boot:raise ValueError(f'missing interpreter live gate: {marker}')
        check=json.loads((ABI/'out/interp/verification.json').read_text())
        output=RESULTS/f'{name}-interp.log';errors=RESULTS/f'{name}-interp-stderr.log'
        if output.read_text()!=check['linux_reference'] or errors.read_text()!=check['linux_stderr']:
            raise ValueError('direct exec output/stderr differs from Linux reference')
        if output.read_text() not in boot or errors.read_text() not in boot:raise ValueError('interpreter logs do not match named boot')
        interp_logs=[('interp',output),('interp-stderr',errors)]
    events=staged.get('linux_events_experiment',False)
    if events:
        for marker in ('NATIVE_EVENT_CONTROL_PASS','LINUX_EVENTS_PASS','EVENTS_EXEC_FD_PASS','XNU_EVENTS_PERSONALITY_PASS'):
            if boot.count(marker)!=1:raise ValueError(f'missing/duplicate event gate: {marker}')
        if not re.search(r'LINUX_EVENTS_RESULT status=0 native_parent=[1-9][0-9]* releases=3 output=',boot):
            raise ValueError('event fixture did not exit zero after three blocking releases')
        for phase in range(1,4):
            for label in ('CHILD','PHASE'):
                if boot.count(f'EVENT_{label}_{phase}\n')!=1:raise ValueError('event blocking handshake mismatch')
    reactor=staged.get('linux_reactor_experiment',False)
    reactor_logs=[]
    if reactor:
        for marker in ('LINUX_PRIVATE_FUTEX_PASS','LINUX_EPOLL_PASS','LINUX_TIMERFD_PASS','LINUX_SIGNALFD_PASS','LINUX_REACTOR_PASS','XNU_REACTOR_CONSTRAINTS_PASS','XNU_REACTOR_PERSONALITY_PASS','ARCH_SD_EVENT_LOOP_PASS','ARCH_SD_EVENT_NATIVE_PARENT_PASS','XNU_SD_EVENT_NATIVE_PARENT_RESULT=0'):
            if boot.count(marker)!=1:raise ValueError(f'missing/duplicate reactor gate: {marker}')
        if not re.search(r'LINUX_REACTOR_RESULT status=0 native_parent=[1-9][0-9]* releases=2 output=',boot):
            raise ValueError('reactor fixture did not exit zero after both forced peer releases')
        check=json.loads((ABI/'out/reactor/verification.json').read_text())
        output=RESULTS/f'{name}-sd-event.log';errors=RESULTS/f'{name}-sd-event-stderr.log'
        expected=check['sd_event_reference']
        if errors.read_text() or not re.fullmatch(re.escape(expected)+r'SD_EVENT_NATIVE_RESULT status=0 parent=[1-9][0-9]*\nARCH_SD_EVENT_NATIVE_PARENT_PASS\n',output.read_text()):
            raise ValueError('Arch libsystemd event-loop output differs from Linux reference')
        if output.read_text() not in boot:raise ValueError('event-loop output is not in named boot')
        reactor_logs=[('sd-event',output),('sd-event-stderr',errors)]
    launch=staged.get('linux_launch_experiment',False)
    if launch:
        for marker in ('NATIVE_LAUNCH_CONTROL_PASS','LINUX_LAUNCH_DESCRIPTORS_PASS','LINUX_LAUNCH_LIMITS_PASS','LINUX_LAUNCH_WAIT_PASS','ARCH_LAUNCH_CHILD_PASS','LINUX_LAUNCH_PASS','XNU_LAUNCH_PERSONALITY_PASS'):
            if boot.count(marker)!=1:raise ValueError(f'missing/duplicate launch gate: {marker}')
        if not re.search(r'LINUX_LAUNCH_RESULT status=0 native_parent=[1-9][0-9]* releases=2 output=',boot):
            raise ValueError('service launch fixture did not pass both forced blocking waits')
        for phase in ('GROUP','EXEC'):
            for label in ('CHILD_READY','WAIT'):
                if boot.count(f'LAUNCH_{phase}_{label}\n')!=1:raise ValueError('launch blocking handshake mismatch')
    subreaper=staged.get('linux_subreaper_experiment',False)
    if subreaper:
        if boot.count('XNU_SUBREAPER_PERSONALITY_PASS')!=1 or boot.count('LINUX_SUBREAPER_RESULT status=0 output=LINUX_SUBREAPER_PASS\n')!=1:
            raise ValueError('native subreaper lifecycle fixture did not pass')
    domain=staged.get('linux_domain_experiment',False)
    if domain:
        if boot.count('XNU_DOMAIN_PERSONALITY_PASS')!=1:
            raise ValueError('native domain lifecycle fixture did not pass')
        cycles=re.findall(r'^NATIVE_DOMAIN_CYCLE_PASS cycle=([12]) key=([1-9][0-9]*)$',boot,re.M)
        if len(cycles)!=2 or [cycle for cycle,key in cycles]!=['1','2'] or cycles[0][1]==cycles[1][1]:
            raise ValueError('domain admission/restart identities are missing or reused')
    manager_result=None
    if process:
        diagnostic=re.search(r'^XNU_MANAGER_ATTEMPT_NATIVE_PARENT_RESULT=(\d+)$',boot,re.M)
        if not diagnostic:raise ValueError('manager-start diagnostic has not completed')
        manager_result=int(diagnostic[1])
    stdout_path=RESULTS/'systemd-version.log';stderr_path=RESULTS/'systemd-version-stderr.log'
    stdout=stdout_path.read_text();stderr=stderr_path.read_text()
    if paths and stderr:raise ValueError(f'path profile systemd stderr is not empty: {stderr}')
    reference=json.loads((ABI/'out/systemd/reference.json').read_text())
    expected=reference['version_output']
    if not stdout.startswith(expected):raise ValueError('systemd stdout differs from Linux reference')
    match=re.fullmatch(r'ARCH_ELF_RESULT child=([1-9][0-9]*) status=0 parent=([1-9][0-9]*)\nARCH_ELF_COMMAND_PASS\n',stdout[len(expected):])
    if not match:raise ValueError('missing successful native-parent result or unexpected stdout')
    if stdout not in boot:raise ValueError('systemd stdout does not match the named boot log')
    proof=ABI/'out'/('init' if subreaper else 'launch' if launch else 'reactor' if reactor else 'events' if events else 'interp' if interp else 'process' if process else 'paths' if paths else 'startup')/'runtime-proof';proof.mkdir(exist_ok=True)
    evidence={}
    logs=[('boot',boot_path),('stdout',stdout_path),('stderr',stderr_path)]+interp_logs+reactor_logs
    if process:logs.append(('manager-attempt',RESULTS/f'{name}-manager-attempt.log'))
    for label,path in logs:
        frozen=proof/f'{name}-{label}.log';shutil.copy2(path,frozen)
        evidence[label]=dict(path=str(frozen.relative_to(ABI)),sha256=digest(frozen))
    return dict(kernel_uuid=staged['kernel_uuid'],collection_sha256=staged['sha256'],
        staged_name=name,domain_membership_runtime_pass=bool(domain),subreaper_runtime_pass=bool(subreaper),launch_runtime_pass=bool(launch),reactor_runtime_pass=bool(reactor),arch_sd_event_loop_runtime_pass=bool(reactor),events_runtime_pass=bool(events),interp_runtime_pass=bool(interp),startup_runtime_pass=True,paths_runtime_pass=bool(paths),process_runtime_pass=bool(process),arch_child_exec_runtime_pass=bool(process),systemd_version_runtime_pass=True,
        native_parent=int(match[2]),linux_child=int(match[1]),
        manager_native_parent_result=manager_result,manager_runtime_pass=False,
        systemd_version=expected.splitlines()[0],stderr_present=bool(stderr),
        stderr=stderr,proof_logs=evidence,
        limits=('Single-thread-per-process fork/exec/wait and systemd --version' if process else 'Single-thread startup and systemd --version only')+'; no PID 1, service-manager, complete filesystem isolation, or physical-driver qualification')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('staged_name')
    args=parser.parse_args()
    result=verify(args.staged_name)
    directory=ABI/'out'/('init' if result['subreaper_runtime_pass'] else 'launch' if result['launch_runtime_pass'] else 'reactor' if result['reactor_runtime_pass'] else 'events' if result['events_runtime_pass'] else 'interp' if result['interp_runtime_pass'] else 'process' if result['process_runtime_pass'] else 'startup')
    (directory/'runtime-verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
