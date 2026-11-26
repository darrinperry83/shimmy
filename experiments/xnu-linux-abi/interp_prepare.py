"""Opt-in direct Arch ELF interpreter using native XNU vnode and map ownership."""
from pathlib import Path
from elf_prepare import replace_once
HERE=Path(__file__).resolve().parent

def overlay(root,edits):
    root=Path(root)
    # Preserve console declarations and every earlier syscall overlay. Replace
    # only the loader/stack section before the errno translator.
    path=root/'bsd/kern/rdxnu_linux_exec.h';before,data=edits[path]
    anchor='/* Linux errno numbers differ from Darwin.'
    loader=(HERE/'interp/loader.inc').read_text()
    start='static bool rd_linux_enabled(void)'
    if data.decode().count(start)!=1 or data.decode().count(anchor)!=1 or loader.count(start)!=1:
        raise ValueError('interpreter loader boundaries changed')
    s=data.decode().split(start)[0]+loader+anchor+data.decode().split(anchor)[1]
    s=replace_once(s,'return copyout(words, *sp, n * sizeof(uint64_t));',
        'error=copyout(words,*sp,n*sizeof(uint64_t));\n    if(!error)rdxnu_console_begin();\n    return error;')
    edits[path]=(before,s.encode())
    path=root/'bsd/kern/rdxnu_elf_plan.h';before,_=edits[path]
    edits[path]=(before,(HERE/'interp/elf_plan.h').read_bytes())
    path=root/'bsd/kern/rdxnu_interp_image.h'
    if path.exists():raise ValueError(f'refusing to replace {path}')
    edits[path]=(None,(HERE/'interp/image.h').read_bytes())
    path=root/'bsd/kern/kern_exec.c';before,data=edits[path];s=data.decode()
    s=replace_once(s,'\tstruct rd_elf_plan rd_plan = {0};','\tstruct rd_elf_plan rd_plan = {0};\n\tstruct rd_interp_image *rd_interp = NULL;')
    s=replace_once(s,'\tchar *                  userspace_coredump_name = NULL;',
        '\tchar *                  userspace_coredump_name = NULL;\n\tif(rd_linux){\n\t\terror=rd_interp_prepare(imgp,&rd_plan,&rd_interp);\n\t\tif(error)goto bad;\n\t}')
    s=replace_once(s,'rd_load_elf(imgp, thread, &map, &load_result, &rd_plan)',
        'rd_load_elf(imgp, thread, &map, &load_result, &rd_plan, rd_interp)')
    s=replace_once(s,'bad:\n\t/* If we hit this, we likely would have leaked an exit reason */',
        'bad:\n\trd_interp_release(rd_interp,imgp->ip_vfs_context);\n\t/* If we hit this, we likely would have leaked an exit reason */')
    edits[path]=(before,s.encode());return edits
