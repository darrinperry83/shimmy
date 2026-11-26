"""Opt-in VM framebuffer console overlay for the established Arch ELF build."""
from pathlib import Path
from elf_prepare import replace_once
HERE = Path(__file__).resolve().parent


def overlay(root, edits):
    path = Path(root) / 'osfmk/console/video_console.c'
    original = path.read_bytes()
    # Pin this otherwise-unmodified source just like the ELF integration.
    import subprocess
    expected = subprocess.check_output(['git', '-C', str(root), 'show',
                                       'HEAD:osfmk/console/video_console.c'])
    if original != expected:
        raise ValueError('console requires unmodified video_console.c')
    edits[path] = (original, original + b'\n' + (HERE / 'console/video_console.inc').read_bytes())
    path = Path(root) / 'bsd/kern/rdxnu_linux_exec.h'
    before, source = edits[path]
    text = source.decode()
    text = replace_once(text, '#define RD_LINUX_FLAG',
        (HERE / 'console/mirror.inc').read_text() + '\n#define RD_LINUX_FLAG')
    text = replace_once(text, 'return copyout(words, *sp, n * sizeof(uint64_t));',
        'error = copyout(words, *sp, n * sizeof(uint64_t));\n'
        '    if (!error) rdxnu_console_begin();\n    return error;')
    text = replace_once(text, 'int error = writev_nocancel(p, &args, &amount);',
        'int error = writev_nocancel(p, &args, &amount);\n'
        '        if (!error && amount > 0) rd_console_mirror_iov(args.fd, args.iovp, args.iovcnt, amount);')
    text = replace_once(text, 'value = error ? -rd_linux_errno(error) : amount;\n    } else if (nr == 93',
        'if (!error && amount > 0) rd_console_mirror(args.fd, args.cbuf, amount);\n'
        '        value = error ? -rd_linux_errno(error) : amount;\n    } else if (nr == 93')
    edits[path] = (before, text.encode())
    return edits
