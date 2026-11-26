# Updating XNU, the Linux personality and Arch userspace

Keep three independently versioned inputs and promote a tested combination.
This is the selected maintenance policy, not a claim that an automated update
pipeline or full systemd compatibility already exists.

| Track | Update input | Required validation |
| --- | --- | --- |
| XNU and Apple environment | Exact XNU source revision, SDK/KDK, Apple OS/driver build and collection identities | Build/link, driver gates, source restoration, native Darwin controls, boot and device tests appropriate to the target. |
| Linux personality | Repository revision and its reviewed ABI implementations, source hooks and regression fixtures | Reused shim contracts plus real Linux comparison and live XNU tests for each supported behavior. |
| Arch Linux ARM userspace | Exact package versions and original binary/library hashes | Loader/libc and systemd gates, followed by service/console tests as implemented. |

The release identity is the tuple of these inputs. XNU does not need a new
Linux syscall implementation merely because a new Linux kernel is released;
follow the published Linux userspace ABI and test the behavior needed by the
supported Arch packages. Conversely, a new Arch package can require an ABI
feature that an older working personality lacks. Upstream Linux source and the
existing shim provide implementation/reference material; importing a new Linux
kernel image is not the personality update mechanism.

## Keep changes reviewable

- Keep personality code in separate files. Limit XNU edits to the necessary
  integration or semantic hooks, and document why each hook is needed.
- Use the existing pinned, temporary overlay workflow: verify expected source,
  apply exact edits, build, and restore the checkout. An upstream source change
  must fail a patch clearly; never silently accept an approximate anchor.
- Reuse the shim's Linux flag, errno, structure and lifecycle contracts and its
  regressions. Kernel ports must replace userspace/JIT/exception-server
  assumptions with appropriate XNU APIs and validate the resulting backend.
- Preserve native Darwin behavior. Linux-specific semantic changes must be
  scoped to Linux processes. The filemap directory-errno change is one example.
- Keep systemd and other Arch binaries unchanged. Improve ABI support when
  their actual requirements expose a gap, and return truthful unsupported errors
  where a feature is outside the supported set.

Small, isolated hooks reduce update work but cannot guarantee source-level
stability inside XNU or compatibility with arbitrary Apple driver collections.
Driver matching remains a release gate. Existing DEVELOPMENT/VMAPPLE diagnostic
passes do not qualify a physical-hardware or production release.

## Promotion procedure

1. Preserve the last passing source/package manifest and boot candidate.
2. Change one input track where possible and build an isolated candidate.
3. Run source/driver gates, targeted regressions and native Darwin controls.
4. Boot the candidate and run the currently implemented Arch/systemd gates.
   A build or Linux-only reference pass cannot promote an XNU runtime gate.
5. Record identities, hashes, results and limitations together, then promote
   the tested combination. Keep the previous combination available for rollback.

The current systemd target and reuse map are in [systemd/README.md](systemd/README.md).
