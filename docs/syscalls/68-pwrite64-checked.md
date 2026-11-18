# ARM64 #68 `pwrite64`: checked guest input

The old core route passed a guest buffer directly to Darwin `pwrite`. Some
guest ELF/JIT mappings cannot be read by a host kernel I/O call, and an invalid
guest address could reach the host write path. The bounded route now validates
the descriptor and offset, copies at most 1 MiB from guest memory into a host
buffer, then performs one positioned write. A failed guest copy returns
`EFAULT` before any host write. The guest file position is unchanged.

This subset accepts writable regular files only. It rejects negative offsets,
reserved/invalid or read-only descriptors, nonregular descriptors, `O_APPEND`,
offset overflow and requests larger than 1 MiB. Large requests are refused
rather than silently truncated. The bridge does not promise Linux behavior
for append-mode positioned writes, nonregular files, or concurrent mutation
of the same file.

Proof: `./build.sh cli`; `python3 tests/run-pwrite64-checked.py --sysroot
<Arch rootfs>` checks the ARM64 guest syscall on a disposable file, including
bad-pointer no-write behavior, contents, unchanged seek position, zero count,
and error cases. A private Arch login/D-Bus smoke test checks boot stability,
not every filesystem and concurrency case.
