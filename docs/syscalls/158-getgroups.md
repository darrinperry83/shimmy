# 158 `getgroups`: checked guest output

The virtual-root path retains the existing synthetic singleton containing the
guest effective GID for systemd compatibility. `size=0` returns one without
touching the pointer; a positive size requires a writable four-byte guest
buffer, otherwise it returns `EFAULT`. Negative sizes return `EINVAL`.

In host-identity mode, the call stages Darwin's group list in host memory and
copies it to checked guest memory, with Linux count/short-buffer behavior. The
host's account groups are observable in that mode. A separate private guest
identity is required for full namespace isolation.

This remains **metadata only** in virtual-root mode. Darwin authorizes host
file operations with host credentials, and `setgroups` currently reports
success without changing the list. The isolated #159 guest-state experiment
was withheld because it would let an app observe a dropped group while host
filesystem access still retained that group's privileges. A complete guest
credential/VFS model is needed before group membership can be represented as
an enforceable security boundary.

The standalone ARM64 probe exercises count, negative size, zero-size pointer
nonaccess, and checked output. A private Arch boot smoke gate is still needed
after integration.
