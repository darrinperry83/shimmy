"""Isolated libc-startup overlay; pins hooks and preserves native field offsets."""
from pathlib import Path
import subprocess
import hashlib
import shutil
import re
from elf_prepare import replace_once
HERE=Path(__file__).resolve().parent


def overlay(root,edits,entry_mode='patched'):
    root=Path(root)
    path=root/'bsd/kern/rdxnu_linux_exec.h'
    before,data=edits[path]
    text=replace_once(data.decode(),'bool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);',
        (HERE/'startup/dispatch.inc').read_text()+'\nbool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);')
    text=replace_once(text,'    if (nr == 25 || nr == 56',
        '    if (nr == 48 || nr == 96 || nr == 134 || nr == 135 || nr == 167 || nr == 178 || nr == 214 || nr == 261 || nr == 278) value = rd_linux_startup(p,state,nr);\n    else if (nr == 25 || nr == 56')
    text=replace_once(text,'if ((flags & ~0x32ULL) || !(flags & 2))',
        'if ((flags & ~0x1832ULL) || !(flags & 2))') # Linux ignores DENYWRITE/EXECUTABLE.
    text=replace_once(text,'        struct exit_args args = {};',
        '''        uint32_t cleared = 0;
        user_addr_t clear_tid = rd_linux_machine_clear_tid(current_thread());
        if (clear_tid) (void)copyout(&cleared,clear_tid,sizeof(cleared));
        rd_linux_machine_set_clear_tid(current_thread(),0);
        struct exit_args args = {};''')
    edits[path]=(before,text.encode())
    path=root/'bsd/kern/kern_exec.c'
    before,data=edits[path]
    text=replace_once(data.decode(),'\tif (rd_linux) os_atomic_or(&p->p_ladvflag, P_RDXNU_LINUX, relaxed);',
        '\trd_linux_state_exec(p, thread, rd_linux, rd_linux ? rd_plan.high : 0);\n\tif (rd_linux) os_atomic_or(&p->p_ladvflag, P_RDXNU_LINUX, relaxed);')
    edits[path]=(before,text.encode())
    path=root/'bsd/kern/kern_fork.c'
    before,data=edits[path]
    text=replace_once(data.decode(),'#include <sys/resourcevar.h>',
        '#include <sys/resourcevar.h>\n#include "rdxnu_linux_state.h"')
    text=replace_once(text,'\tchild_proc->p_stats = zalloc_flags(proc_stats_zone, Z_WAITOK | Z_ZERO);',
        '\tchild_proc->p_stats = zalloc_flags(proc_stats_zone, Z_WAITOK | Z_ZERO);\n\trd_linux_state_fork(parent_proc, child_proc);')
    text=replace_once(text,'\tzfree(proc_stats_zone, p->p_stats);',
        '\trd_linux_state_free(p);\n\tzfree(proc_stats_zone, p->p_stats);')
    edits[path]=(before,text.encode())
    changes={
        'bsd/kern/kern_exit.c': [('#include <sys/resourcevar.h>',
                                '#include <sys/resourcevar.h>\n#include "rdxnu_linux_state.h"'),
                               ('\tzfree(proc_stats_zone, p->p_stats);',
                                '\trd_linux_state_free(p);\n\tzfree(proc_stats_zone, p->p_stats);')],
        'bsd/sys/resourcevar.h': [('\t} user_p_prof;\n#endif // XNU_KERNEL_PRIVATE',
            '\t} user_p_prof;\n\tstruct rd_linux_proc_state *ps_rd_linux; /* Private Linux sidecar; existing offsets unchanged. */\n#endif // XNU_KERNEL_PRIVATE')],
        'osfmk/arm64/machine_machdep.h': [('#define ARM_MACHINE_THREAD_PRESERVE_X18_INITIAL_SHIFT   3',
            '#define ARM_MACHINE_THREAD_RDXNU_LINUX_SHIFT 31\n#define ARM_MACHINE_THREAD_RDXNU_LINUX (1U << ARM_MACHINE_THREAD_RDXNU_LINUX_SHIFT)\n\n#define ARM_MACHINE_THREAD_PRESERVE_X18_INITIAL_SHIFT   3')],
        'osfmk/arm64/genassym.c': [('\tDECLARE("TH_ARM_MACHINE_FLAGS", offsetof(struct thread, machine.arm_machine_flags));',
            '\tDECLARE("TH_ARM_MACHINE_FLAGS", offsetof(struct thread, machine.arm_machine_flags));\n\tDECLARE("ACT_RDXNU_TLS", offsetof(struct thread, machine.reserved4));')],
        'osfmk/conf/files.arm64': [('osfmk/arm64/locore.s\toptional nos_arm_asm',
                                 'osfmk/arm64/locore.s\tstandard')],
        'makedefs/MakeInc.def': [('-Wl,-force_load,$(KDKROOT)/System/Library/KernelSupport/lib$(CURRENT_MACHINE_CONFIG).os.$(CURRENT_KERNEL_CONFIG).a',
            '-Wl,-force_load,$(if $(RDXNU_KERNEL_SUPPORT_ARCHIVE),$(RDXNU_KERNEL_SUPPORT_ARCHIVE),$(KDKROOT)/System/Library/KernelSupport/lib$(CURRENT_MACHINE_CONFIG).os.$(CURRENT_KERNEL_CONFIG).a)')],
        'osfmk/arm64/locore.s': [
            ('\tmrs\t\tx1, TPIDR_EL1\t\t\t\t\t\t// Load the thread register\n\n\n\n#if HAS_ARM_FEAT_SME',
             '''\tmrs\t\tx1, TPIDR_EL1\t\t\t\t\t\t// Load the thread register
\t/* Linux user TLS is saved before any kernel CPU bookkeeping uses TPIDR_EL0. */
\tldr w0, [x1, TH_ARM_MACHINE_FLAGS]
\ttbz w0, ARM_MACHINE_THREAD_RDXNU_LINUX_SHIFT, 9f
\tmrs x0, TPIDR_EL0
\tstr x0, [x1, ACT_RDXNU_TLS]
\tldr x0, [x1, ACT_CPUDATAP]
\tldr x0, [x0, CPU_TPIDR_EL0]
\tmsr TPIDR_EL0, x0
9:

#if HAS_ARM_FEAT_SME'''),
            ('\tmov\t\tx18, #0\n\tmrs\t\tx1, TPIDR_EL0',
             '''\tldr w1, [x3, TH_ARM_MACHINE_FLAGS]
\ttbnz w1, ARM_MACHINE_THREAD_RDXNU_LINUX_SHIFT, exception_return_unint_tpidr_x3_restore_x18
\tmov\t\tx18, #0
\tmrs\t\tx1, TPIDR_EL0'''),
            ('\t// Skip x0, x1 - we\'re using them\n\tldp\t\tx2, x3, [x0, SS64_X2]',
             '''\t/* Restore TLS only on return to Linux EL0, after kernel bookkeeping. */
\tmrs x1, SPSR_EL1
\ttbnz x1, PSR64_MODE_EL_SHIFT, 9f
\tmrs x1, TPIDR_EL1
\tldr w2, [x1, TH_ARM_MACHINE_FLAGS]
\ttbz w2, ARM_MACHINE_THREAD_RDXNU_LINUX_SHIFT, 9f
\tldr x1, [x1, ACT_RDXNU_TLS]
\tmsr TPIDR_EL0, x1
9:
\t// Skip x0, x1 - we're using them
\tldp\t\tx2, x3, [x0, SS64_X2]''')],
    }
    if entry_mode=='stock':
        for relative in ('osfmk/arm64/locore.s','osfmk/arm64/genassym.c','osfmk/conf/files.arm64','makedefs/MakeInc.def'):
            del changes[relative]
    elif entry_mode=='source-control':
        changes['osfmk/arm64/locore.s']=[]
    elif entry_mode!='patched':
        raise ValueError('unknown startup entry mode')
    for relative,replacements in changes.items():
        path=root/relative;before=path.read_bytes()
        expected=subprocess.check_output(['git','-C',str(root),'show',f'HEAD:{relative}'])
        if before!=expected:raise ValueError(f'startup requires unmodified {relative}')
        text=before.decode()
        for old,new in replacements:text=replace_once(text,old,new)
        edits[path]=(before,text.encode())
    path=root/'osfmk/arm64/machine_routines.c'
    before=path.read_bytes()
    expected=subprocess.check_output(['git','-C',str(root),'show','HEAD:osfmk/arm64/machine_routines.c'])
    if before!=expected:raise ValueError('startup requires unmodified machine_routines.c')
    edits[path]=(before,before+b'\n#include "rdxnu_linux_machine.inc"\n')
    path=root/'osfmk/arm64/rdxnu_linux_machine.inc'
    if path.exists():raise ValueError(f'refusing to replace {path}')
    edits[path]=(None,(HERE/'startup/machine.inc').read_bytes())
    path=root/'bsd/kern/rdxnu_linux_state.h'
    if path.exists():raise ValueError(f'refusing to replace {path}')
    edits[path]=(None,(HERE/'startup/state.h').read_bytes())
    return edits


def support_archive(target,kdk):
    """Replace only the prebuilt locore member with our source-built object."""
    source=Path(kdk)/'System/Library/KernelSupport/libVMAPPLE.os.DEVELOPMENT.a'
    destination=Path(target)/'libVMAPPLE.without-locore.a'
    members=subprocess.check_output(['xcrun','ar','-t',str(source)],text=True).splitlines()
    if members.count('locore.o')!=1:raise ValueError('expected one locore support object')
    shutil.copy2(source,destination)
    subprocess.run(['xcrun','ar','-d',str(destination),'locore.o'],check=True)
    remaining=subprocess.check_output(['xcrun','ar','-t',str(destination)],text=True).splitlines()
    expected=[name for name in members if name not in ('locore.o','__.SYMDEF SORTED','__.SYMDEF')]
    actual=[name for name in remaining if not name.startswith('__.SYMDEF')]
    if expected!=actual:raise ValueError('unexpected support archive member changes')
    for name in expected:
        old=subprocess.check_output(['xcrun','ar','-p',str(source),name])
        new=subprocess.check_output(['xcrun','ar','-p',str(destination),name])
        if old!=new:raise ValueError(f'support member changed: {name}')
    return destination,dict(original_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        modified_sha256=hashlib.sha256(destination.read_bytes()).hexdigest(),
        removed_member='locore.o',remaining_members_byte_identical=True)


def verify_offsets(target):
    """Reject accidental native assembly ABI changes against the working gate."""
    control=HERE/'out/vmapple/baseline-no-lto-no-bti-svc-probe-elf-arch-loader-linux-console-terminal-memory-filemap-files'
    relative='obj/DEVELOPMENT_ARM64_VMAPPLE/osfmk/DEVELOPMENT/assym.s'
    def defines(path):
        return dict(re.findall(r'^#define\s+(\w+)\s+([^\n]+)',path.read_text(),re.M))
    before=defines(control/relative);after=defines(Path(target)/relative)
    if not before or 'TH_ARM_MACHINE_FLAGS' not in before:raise ValueError('native offset control missing')
    changed={key:(value,after.get(key)) for key,value in before.items() if after.get(key)!=value}
    if changed:raise ValueError(f'native assembly definitions changed: {changed}')
    if 'ACT_RDXNU_TLS' not in after:raise ValueError('Linux TLS offset missing')
    return dict(control_definitions=len(before),existing_definitions_unchanged=True,
                linux_tls_offset=after['ACT_RDXNU_TLS'])
