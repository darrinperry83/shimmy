# Interactive Linux shell bring-up

Architecture steering: [XNU serves the Linux ABI](../LINUX-PERSONALITY.md).
XNU owns memory, task/thread execution and Apple devices, reusing the shim's
compatibility contracts inside the kernel personality. The hosted Linux-core
experiment is separate. The existing ELF/PTY path is the shell foundation.


The committed console checkpoint is `bb4af5d` on development. This next variant
adds Linux AArch64 read (63) through XNU's native descriptor/tty implementation.
It preserves the original Arch-loader and visible-console candidates in separate
output directories. No shim syscall interception is involved.

## First terminal test

`read_probe.S` is an ordinary static Linux ELF. A native parent creates a PTY,
sets canonical input and echo, makes it the child's controlling terminal, and
executes the ELF. The parent supplies `abc<erase>D<newline>` and then Ctrl-D.
The ELF checks EBADF, zero-length read, EFAULT, the resulting `abD\n` line, and
EOF. The parent requires a zero child status and unchanged native process ID.
The same fixture/parent pair passes on real AArch64 Linux in Docker. This is a
small input compatibility gate, not a shell or Apple HID keyboard implementation.
The line discipline is XNU's tty; Linux termios/ioctl translation is still needed.

```
python3 experiments/xnu-linux-abi/terminal/verify.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-terminal
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-terminal
```

The fixture and native parent are written to `out/terminal`. Kernel artifacts are
in `out/vmapple/baseline-no-lto-no-bti-svc-probe-elf-arch-loader-linux-console-terminal`.
Build/link does not install or boot. The existing DEVELOPMENT/VMAPPLE-only,
unsigned-ELF and no-BTI diagnostic limitations remain. `guest_boot.sh` checks the
live UUID, earlier P0/M1/Arch-loader controls and PTY test, then presents the PTY
transcript using the proven native AppleParavirtDisplay output host. It exports
`terminal-boot.log` and `terminal-display.log` through the VM share.

## Real Arch Bash target

`bash-reference.json` records the package identity and syscall numbers observed
while the original Arch Bash 5.3.20-1 runs startup and builtins on real Linux.
`trace_shell.gdb` reproduces the trace. This is noninteractive coverage only;
not every observed syscall must succeed, and a number alone does not describe
its required flags, structure layout or semantics. Bash has not run on XNU yet.

Next stages, each with Linux reference tests and live XNU checks:

1. Streaming terminal rendering and scrollback; Apple HID keyboard events routed
   into the PTY master. Prove text entry and erase on the visible guest screen.
2. Translate Linux terminal ioctls/termios and window size; test raw/canonical
   modes, echo, EOF, nonblocking reads and Ctrl-C. Preserve the working output host.
3. Run the original Arch loader against a real dynamic executable. Implement and
   validate brk/mmap/mprotect/munmap, file open/read/close/metadata, TLS/thread
   initialization, credentials and random data according to measured needs.
4. Run Arch Bash builtins; implement Linux signal frames/actions/masks and process
   groups, with default signals and interrupted reads verified independently.
5. Fork/exec/wait, pipes, redirection and foreground/background jobs. A working
   prompt is insufficient: verify external Arch commands, pipelines, Ctrl-C,
   Ctrl-Z/fg, exit statuses, terminal restoration and repeated sessions.

The native display/PTY frontend remains Darwin code using Apple drivers; shell
and commands are Linux ELF processes using the kernel Linux personality. Keeping
the platform frontend native does not substitute a Darwin shell for Arch Bash.

## XNU runtime result

The same fixture passes on the live custom VM kernel
`713AB341-1CF9-3E1A-8601-E2D8F75E1EAF`. The ELF hash matches the real-Linux
reference. P0, M1 and original Arch-loader controls pass in the same boot, and
the PTY transcript was visually verified on the Apple display output path.
See `evidence.json`. Input is supplied by the test harness; physical keyboard
input and a running Bash session remain unproven. The bitmap output host does
not yet interpret terminal control characters such as echoed backspace; this
visible rendering limitation is separate from the verified canonical input data.
