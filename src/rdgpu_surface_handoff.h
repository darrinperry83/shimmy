#ifndef RDGPU_SURFACE_HANDOFF_H
#define RDGPU_SURFACE_HANDOFF_H
#include <stdint.h>
#include <sys/types.h>
#include <IOSurface/IOSurface.h>

/* Cross-process dma-buf fd -> IOSurface handoff.
 *
 * The rdmetal client (shim_cli) creates an IOSurface in its own process and
 * exports a virtual dma-buf carrier fd over zwp_linux_dmabuf_v1. The Hyprland
 * compositor (shim_gui, a DIFFERENT process) receives that fd via SCM_RIGHTS
 * and must resolve it back to the SAME IOSurface.
 *
 * An AF_UNIX SCM_RIGHTS fd cannot carry a mach port, so the send-right from
 * IOSurfaceCreateMachPort is delivered on a separate authenticated channel
 * keyed by the carrier's fstat(st_dev, st_ino) identity. The importing side
 * redeems it with IOSurfaceLookupFromMachPort and stores the CFRetained
 * IOSurfaceRef in the DRM gem slot (see drmasahi.c).
 *
 * Ownership contract:
 *  - rdgpu_handoff_publish() takes a borrowed surface and keeps an internal
 *    CFRetained reference (plus the mach send-right) until revoke/destroy.
 *  - rdgpu_handoff_resolve_fd() returns a NEW +1 reference the caller owns
 *    (CFRelease it), or NULL when the fd matches no published surface or the
 *    cross-process transport is not installed. */

/* Export side (shim_cli, rdgpu_mock_gate.c mock_fd_export). Returns 0 on
 * success or a negative errno. */
int rdgpu_handoff_publish(IOSurfaceRef surface, dev_t device, ino_t inode);

/* Import side (shim_gui): one-time init that allocates the receive right,
 * registers it under the per-run bootstrap name, and starts the receive
 * thread. Idempotent (pthread_once); safe to call eagerly at compositor
 * startup (wlsrv_start) and lazily from resolve_fd. */
void rdgpu_handoff_import_init(void);

/* Import side (shim_gui, drmasahi.c PRIME_FD_TO_HANDLE). Returns a CFRetained
 * IOSurfaceRef for the imported carrier fd, or NULL. */
IOSurfaceRef rdgpu_handoff_resolve_fd(int fd);

/* Export side: drop a published surface (carrier released/destroyed). */
void rdgpu_handoff_revoke(dev_t device, ino_t inode);

#endif
