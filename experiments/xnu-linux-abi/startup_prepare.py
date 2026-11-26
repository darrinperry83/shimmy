"""Isolated libc-startup overlay; pins hooks and preserves native field offsets."""
from pathlib import Path
import subprocess
import re
from elf_prepare import replace_once
HERE=Path(__file__).resolve().parent


def overlay(root,edits,entry_mode='kdk-bridge'):
    root=Path(root)
    path=root/'bsd/kern/rdxnu_linux_exec.h'
    before,data=edits[path]
    text=replace_once(data.decode(),'bool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);',
        (HERE/'startup/dispatch.inc').read_text()+'\nbool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);')
    text=replace_once(text,'    if (nr == 25 || nr == 56',
        '    if (nr == 48 || nr == 96 || nr == 113 || nr == 114 || nr == 134 || nr == 135 || nr == 167 || nr == 178 || nr == 214 || nr == 261 || nr == 278) value = rd_linux_startup(p,state,nr);\n    else if (nr == 25 || nr == 56')
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
        'bsd/sys/user.h': [('#endif /* CONFIG_DEBUG_SYSCALL_REJECTION */\n};',
            '#endif /* CONFIG_DEBUG_SYSCALL_REJECTION */\n\t/* Owned Linux tail; never reuse private machine reserved fields. */\n\tuint64_t rd_linux_tls, rd_linux_clear_tid, rd_linux_mask, rd_linux_enabled;\n};')],
        'osfmk/arm64/genassym.c': [('\tDECLARE("TH_ARM_MACHINE_FLAGS", offsetof(struct thread, machine.arm_machine_flags));',
             '\tDECLARE("TH_ARM_MACHINE_FLAGS", offsetof(struct thread, machine.arm_machine_flags));\n'
             '\tDECLARE("ACT_RDXNU_THREAD_SIZE", sizeof(struct thread));')],
        'osfmk/conf/files.arm64': [('osfmk/arm64/locore.s\toptional nos_arm_asm',
            'osfmk/arm64/locore.s\toptional nos_arm_asm\nosfmk/arm64/rdxnu_linux_entry.s\tstandard')],
        'makedefs/MakeInc.def': [('-Wl,-force_load,$(KDKROOT)/System/Library/KernelSupport/lib$(CURRENT_MACHINE_CONFIG).os.$(CURRENT_KERNEL_CONFIG).a',
            '-Wl,-force_load,$(if $(RDXNU_KERNEL_SUPPORT_ARCHIVE),$(RDXNU_KERNEL_SUPPORT_ARCHIVE),$(KDKROOT)/System/Library/KernelSupport/lib$(CURRENT_MACHINE_CONFIG).os.$(CURRENT_KERNEL_CONFIG).a)')],
    }
    if entry_mode=='stock':
        for relative in ('osfmk/arm64/genassym.c','osfmk/conf/files.arm64','makedefs/MakeInc.def'):
            del changes[relative]
    elif entry_mode!='kdk-bridge':
        raise ValueError('legacy entry replacement rejected; use stock or kdk-bridge')
    for relative,replacements in changes.items():
        path=root/relative;before=path.read_bytes()
        expected=subprocess.check_output(['git','-C',str(root),'show',f'HEAD:{relative}'])
        if before!=expected:raise ValueError(f'startup requires unmodified {relative}')
        text=before.decode()
        for old,new in replacements:text=replace_once(text,old,new)
        edits[path]=(before,text.encode())
    if entry_mode=='kdk-bridge':
        path=root/'osfmk/arm64/rdxnu_linux_entry.s'
        if path.exists():raise ValueError(f'refusing to replace {path}')
        edits[path]=(None,(HERE/'startup/entry.s').read_bytes())
    path=root/'bsd/kern/rdxnu_linux_machine.inc'
    if path.exists():raise ValueError(f'refusing to replace {path}')
    edits[path]=(None,(HERE/'startup/machine.inc').read_bytes())
    path=root/'bsd/kern/rdxnu_linux_state.h'
    if path.exists():raise ValueError(f'refusing to replace {path}')
    edits[path]=(None,(HERE/'startup/state.h').read_bytes())
    return edits


def support_archive(target,kdk):
    from startup_archive import bridge_archive
    return bridge_archive(Path(target),Path(kdk))


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
    if 'ACT_RDXNU_THREAD_SIZE' not in after:raise ValueError('native thread size missing')
    return dict(control_definitions=len(before),existing_definitions_unchanged=True,
                native_thread_size=after['ACT_RDXNU_THREAD_SIZE'])
