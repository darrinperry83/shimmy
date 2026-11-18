"""Extract numbered syscall macros from an installed native AArch64 header.

The generic header's time64 block is compiled only for compat or 32-bit
callers. This deliberately small parser handles that guard and its nesting;
other preprocessor conditions remain a source-audit limitation.
"""
import re

NUMBER = re.compile(r"^#define __NR_(\w+)\s+(\d+)\s*$")
COMPAT_GUARD = re.compile(
    r"^#if\s+defined\(__SYSCALL_COMPAT\)\s*\|\|\s*__BITS_PER_LONG\s*==\s*32\s*$"
)
OPEN = re.compile(r"^#\s*(?:if|ifdef|ifndef)\b")
CLOSE = re.compile(r"^#\s*endif\b")
ELSE = re.compile(r"^#\s*else\b")
ELIF = re.compile(r"^#\s*elif\b")


def numbered_native_entries(text):
    """Return (native number→name, compat-only number→name)."""
    native, compat = {}, {}
    depth = 0
    skip = False
    for line in text.splitlines():
        line = line.strip()
        if depth:
            if OPEN.match(line):
                depth += 1
            elif CLOSE.match(line):
                depth -= 1
                if not depth:
                    skip = False
                continue
            elif depth == 1 and ELSE.match(line):
                skip = False
                continue
            elif depth == 1 and ELIF.match(line):
                raise ValueError("unsupported branch in compat-only syscall guard")
        elif COMPAT_GUARD.match(line):
            depth, skip = 1, True
            continue
        match = NUMBER.match(line)
        if match and match.group(1) != "syscalls":
            number, name = int(match.group(2)), match.group(1)
            (compat if skip else native).setdefault(number, name)
    if depth:
        raise ValueError("unterminated compat-only syscall guard")
    return native, compat
