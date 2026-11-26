# Maintaining Arch's Linux core on XNU — no VM

This is the selected update architecture, not a completed kernel port or build
service. Runtime remains native Linux ELF processes hosted by XNU. No hypervisor,
CPU emulation, stock Linux Image guest, or per-syscall Darwin implementation is
introduced by this plan.

## Two source tracks and one tested release bundle

Maintain the Linux core and the Apple platform independently:

- **Linux track:** follow Arch Linux ARM's `linux-aarch64` source recipe, upstream
  kernel, Arch patch/config identities, and our hosted architecture patch stack.
- **Apple track:** pin XNU source, board/KDK/driver collection, host backend, and
  broker. Change this track for Apple platform fixes and supported Apple updates,
  rather than simply because Arch releases another Linux kernel.
- **Release bundle:** select one tested Linux core + XNU/backend/driver set +
  matching userspace snapshot. Never infer compatibility from version numbers.

We do not merge all Linux source into Apple's source tree. Keep it separately
buildable. The initial target artifact is a namespaced freestanding Mach-O object
or archive linked into the custom XNU build. It must not depend on libSystem or
the POSIX/pthread backend of a userspace LKL demonstration. Linux updates can
reuse unchanged XNU object outputs and drivers, then relink/repackage/retest the
candidate. This still creates a new boot collection; it is not a hot-swap of the
running kernel. A separately loadable core is a later design, not a dependency
or a promise of maintenance-free live updates.

## A narrow host interface, owned by this project

The proposed host interface exposes mechanisms, not Linux internal structures:

- opaque task/thread/address-space/buffer handles;
- checked user-memory access, mapping/fault/COW lifecycle primitives;
- execution context entry/return, thread blocking/wakeup, clocks/timers;
- device transport, shared-buffer lifetime, fences and notifications.

Use an explicit ABI major/minor, structure size, fixed-width fields and feature
negotiation. Keep Linux's `task_struct`, `mm_struct`, VMA, file, socket, cgroup and
scheduler internals inside the Linux build. Keep XNU private structs behind the
XNU backend. No raw private structs shared between independently built sides.
Linux-side glue may change with Linux; XNU-side glue may change with Apple.
The interface limits how far each change propagates. It does not remove the
Linux-side port, solve mapping ownership, or establish compatibility by itself.

Linux owns the intended syscall semantics, root filesystem and networking stack.
The host backend should not become a renamed table of Darwin read/open/epoll/etc.
implementations. Native Apple device services are accessed through device-level
interfaces. Existing console/PTY proofs remain controls, not the production core.

## Routine Linux update procedure

1. Pin the actual Arch recipe revision, source/patch/config hashes and toolchain.
   Produce a source intake and change report. A recipe/source mismatch blocks
   the candidate; a plain kernel Image is not a hosted-port update input.
2. Replay the small, ordered hosted-port patch stack in a fresh checkout. Abort
   on conflicts. Run configuration migration and report new/unset options; do
   not blindly copy the stock hardware ARM64 config onto the hosted target.
3. Build Linux independently against a supported host-interface version, then
   relink against the cached, unchanged XNU platform build where compatible.
   Record both source identities, configuration, compiler and final artifact hash.
4. Run Linux semantic comparisons and native XNU checks: process/memory/signals,
   threads/futex/SMP, files/epoll/networking, terminal/job control, namespaces and
   cgroups. Boot original Arch systemd as Linux-domain PID 1 and run real services.
   Keep Darwin/Apple service and display/input regressions in the acceptance set.
5. Publish a **candidate** custom Arch package/bundle only after those checks.
   Stage it alongside the known-good release. Activate at a controlled reboot,
   record the actual identities and health result, and preserve a tested rollback.

An automatic watcher/build pipeline is a future implementation. Do not schedule
or enable unattended activation merely because this procedure is documented.

## Routine XNU update procedure

Leave Linux's revision unchanged initially. Update the XNU backend and matching
Apple driver/firmware collection; test the host-interface contract, rebuild the
compatible bundle, and repeat the same runtime checks. This isolates Apple-side
failures from Linux rebases. Only then qualify combinations with newer Linux.

## Packaging and what “latest Arch” means

Provide a project package such as `rdxnu-linux-core` plus a matched platform
bundle, rather than letting the normal `linux-aarch64` install hooks replace XNU
with an Image. Arch userspace remains genuine Arch packages. The custom core
package records the exact Arch/Linux source base and host ABI it supports.
Package naming, provides/conflicts, repositories and hooks still require actual
implementation and install/upgrade/rollback tests before use.

Maintain two statuses: **newest Arch source observed** and **newest Arch source
verified on our port**. Show any gap explicitly. Every Arch release can trigger
a candidate build; it cannot be called supported until the tests pass. Security
updates get priority, but do not bypass compatibility checks. An LKL-derived base
that remains on an older kernel is a reuse candidate, not evidence that we already
follow current Arch.

## What maintenance remains

Linux does not offer a stable internal kernel API. Rebases may require changes
in our architecture code, especially around tasks, memory, signals, scheduling
and locking. A versioned project interface protects XNU from unnecessary churn;
it cannot make upstream Linux internals stable. Reduce this work through a small
patch stack, few generic-kernel edits, continuous testing, and upstreaming usable
port work where accepted. Zero-maintenance tracking of arbitrary releases is not
an acceptance claim.

The first proof of this update architecture is **two Linux source revisions**
passing the same Arch/systemd + Darwin checks on one unchanged XNU platform build,
followed by a successful rollback. Source intake alone does not satisfy it.

References:
- https://docs.kernel.org/process/stable-api-nonsense.html
- https://github.com/archlinuxarm/PKGBUILDs/tree/master/core/linux-aarch64
