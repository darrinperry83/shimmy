"""XNU-backed Linux memory personality overlay; independent of hosted Linux."""
from pathlib import Path
from elf_prepare import replace_once
HERE = Path(__file__).resolve().parent


def overlay(root, edits):
    path = Path(root) / 'bsd/kern/rdxnu_linux_exec.h'
    before, source = edits[path]
    text = replace_once(source.decode(), 'bool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);',
                        (HERE / 'memory/dispatch.inc').read_text() + '\nbool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);')
    text = replace_once(text, '    if (nr == 63) {',
                        '    if (nr == 222 || nr == 226 || nr == 215) value = rd_linux_memory(p,state,nr);\n    else if (nr == 63) {')
    edits[path] = (before, text.encode())
    return edits
