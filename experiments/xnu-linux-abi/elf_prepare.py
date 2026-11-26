"""Exact-source, temporary M1 integration for the pinned XNU VM checkout."""
from pathlib import Path
import subprocess
import prepare

HERE = Path(__file__).resolve().parent


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'ELF patch anchor not unique: {old[:100]!r}')
    return text.replace(old, new, 1)


def edits(root, arch_loader=False):
    root = Path(root)
    if subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip() != prepare.COMMIT:
        raise ValueError('unexpected XNU revision')
    result = {}
    for relative in ['bsd/kern/kern_exec.c', 'bsd/kern/kern_fork.c', 'bsd/sys/proc_internal.h']:
        path = root / relative
        original = subprocess.check_output(['git', '-C', str(root), 'show', f'HEAD:{relative}'])
        if path.read_bytes() != original:
            raise ValueError(f'ELF requires unmodified {relative}')
        text = original.decode()
        if relative.endswith('proc_internal.h'):
            text = replace_once(text, '#define P_RSR                   0x04',
                '#define P_RSR                   0x04\n#define P_RDXNU_LINUX           0x08 /* M1 experimental Linux personality */')
        elif relative.endswith('kern_fork.c'):
            anchor = '\tchild_proc->p_flag = (parent_proc->p_flag & (P_LP64'
            index = text.rfind('#if CONFIG_DELAY_IDLE_SLEEP', 0, text.index(anchor))
            if index < 0: raise ValueError('fork flag anchor missing')
            # Inherit only our reserved personality bit; keep existing fork rules.
            text = text[:index] + '\tos_atomic_or(&child_proc->p_ladvflag,\n\t    parent_proc->p_ladvflag & P_RDXNU_LINUX, relaxed);\n' + text[index:]
        else:
            text = replace_once(text, 'static int\nexec_mach_imgact(struct image_params *imgp)',
                '#define IMGPF2_RDXNU_ELF 0x04\n#include "rdxnu_linux_exec.h"\n\nstatic int\nexec_mach_imgact(struct image_params *imgp)')
            text = replace_once(text, '\tstruct mach_header *mach_header = (struct mach_header *)imgp->ip_vdata;',
                '\tstruct mach_header *mach_header = (struct mach_header *)imgp->ip_vdata;\n'
                '\tstruct mach_header rd_header = {0};\n'
                '\tstruct rd_elf_plan rd_plan = {0};\n'
                '\tbool rd_linux = !memcmp(imgp->ip_vdata, "\\177ELF", 4);\n'
                '\tif (rd_linux) {\n'
                '\t\tif (!rd_linux_enabled() || rd_elf_plan(imgp->ip_vdata, PAGE_SIZE,\n'
                '\t\t    imgp->ip_vattr->va_data_size, &rd_plan)) return ENOEXEC;\n'
                '\t\timgp->ip_flags2 |= IMGPF2_RDXNU_ELF;\n'
                '\t\t/* Descriptor for shared exec housekeeping; ELF file bytes stay unchanged. */\n'
                '\t\trd_header.magic = MH_MAGIC_64;\n'
                '\t\trd_header.cputype = CPU_TYPE_ARM64;\n'
                '\t\trd_header.cpusubtype = CPU_SUBTYPE_ARM64_ALL;\n'
                '\t\trd_header.filetype = MH_EXECUTE;\n'
                '\t\tmach_header = &rd_header;\n'
                '\t}')
            text = replace_once(text, '\tif (validate_potential_simulator_binary(imgp->ip_origcputype, imgp,',
                '\tif (!rd_linux && validate_potential_simulator_binary(imgp->ip_origcputype, imgp,')
            text = replace_once(text, '\tlret = load_machfile(imgp, mach_header, thread, &map, &load_result);',
                '\tlret = rd_linux ? rd_load_elf(imgp, thread, &map, &load_result, &rd_plan) : load_machfile(imgp, mach_header, thread, &map, &load_result);')
            text = replace_once(text, '\terror = exec_add_apple_strings(imgp, &load_result, task);',
                '\terror = rd_linux ? 0 : exec_add_apple_strings(imgp, &load_result, task);')
            text = replace_once(text, '\t\terror = exec_copyout_strings(imgp, &ap);',
                '\t\terror = rd_linux ? rd_linux_stack(imgp, &ap, &rd_plan) : exec_copyout_strings(imgp, &ap);')
            text = replace_once(text, '\tOSBitOrAtomic(P_EXEC, &p->p_flag);',
                '\tif (rd_linux) os_atomic_or(&p->p_ladvflag, P_RDXNU_LINUX, relaxed);\n'
                '\telse os_atomic_andnot(&p->p_ladvflag, P_RDXNU_LINUX, relaxed);\n'
                '\tOSBitOrAtomic(P_EXEC, &p->p_flag);')
            text = replace_once(text, 'process_signature(proc_t p, struct image_params *imgp)\n{',
                'process_signature(proc_t p, struct image_params *imgp)\n{\n'
                '\t/* Unsigned Linux images are accepted only by this opt-in VM experiment.\n'
                '\t * Retain the MAC policy errors already collected by exec. */\n'
                '\tif (imgp->ip_flags2 & IMGPF2_RDXNU_ELF) {\n'
                '\t\tif (!rd_linux_enabled() || imgp->ip_mac_return ||\n'
                '\t\t    imgp->ip_cs_error != OS_REASON_NULL) return EACCES;\n'
                '\t\tproc_csflags_update(p, 0);\n'
                '\t\tvm_map_cs_enforcement_set(get_task_map(proc_task(p)), FALSE);\n'
                '\t\treturn 0;\n'
                '\t}')
        result[path] = (original, text.encode())
    for name, source in [('rdxnu_elf_plan.h', 'elf_plan.h'), ('rdxnu_linux_exec.h', 'linux_exec.h')]:
        path = root / 'bsd/kern' / name
        if path.exists(): raise ValueError(f'refusing to replace {path}')
        result[path] = (None, (HERE / ('arch-loader' if arch_loader else 'elf') / source).read_bytes())
    return result
