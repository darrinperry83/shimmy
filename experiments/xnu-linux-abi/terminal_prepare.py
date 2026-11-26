"""Isolated Linux terminal syscall overlay; keeps the console checkpoint intact."""
from pathlib import Path
from elf_prepare import replace_once


def overlay(root, edits):
    path = Path(root) / 'bsd/kern/rdxnu_linux_exec.h'
    before, data = edits[path]
    text = replace_once(data.decode(), '    if (nr == 172) value = proc_getpid(p);', '''    if (nr == 63) {
        struct read_nocancel_args args = {};
        args.fd = (int)get_saved_state_reg(state, 0);
        args.cbuf = get_saved_state_reg(state, 1);
        args.nbyte = get_saved_state_reg(state, 2);
        user_ssize_t amount = 0;
        /* Use XNU's descriptor/tty path: blocking, EOF and descriptor flags
         * belong to the open file, not to a second Linux-side input queue. */
        int error = args.nbyte > INT64_MAX ? EINVAL : read_nocancel(p, &args, &amount);
        value = error ? -rd_linux_errno(error) : amount;
    } else if (nr == 172) value = proc_getpid(p);''')
    edits[path] = (before, text.encode())
    return edits
