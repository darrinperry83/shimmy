# `fgetxattr` (#10)

The old handler passed the guest name and value pointers directly to Darwin.
That bypassed Shimmy's guest-memory checks and could fail for static ELF or
executable mappings even when the Linux guest address was valid.

This bounded ARM64 path snapshots at most 255 name bytes plus a NUL using
checked guest copyin. It reads the xattr into host memory and uses checked
copyout for exactly the bytes returned. A requested size of zero remains a
size query with no guest output. Staging is capped at 1 MiB; Linux xattr
values are at most 64 KiB, so an oversized caller buffer can still retrieve
a valid value. A host attribute over the cap may return `ERANGE` even if the
guest requested more space. The handler preserves host errors for missing
attributes, invalid descriptors and buffers too small for the value.

The disposable ARM64 probe creates one file with a host xattr, then checks an
executable static name, size query, stack and static value buffers, `ERANGE`
without partial guest output, invalid and read-only destinations, malformed
name, and a large declared capacity. It does not cover the related `getxattr`
and `lgetxattr` path calls or the set/list/remove xattr families, whose guest
pointer handling still needs separate work.

The isolated CLI build, ARM64 probe and private Arch LOGIN/D-Bus boot passed;
the boot runner stopped its exact guest and the disposable clone was cleaned.
