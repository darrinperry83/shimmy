// rdgpu_macsurface_client.h — shim-side client for the WindowServer-injected
// mac-surface resolver (com.rdgpu.macsurface).

#ifndef RDGPU_MACSURFACE_CLIENT_H
#define RDGPU_MACSURFACE_CLIENT_H

#include <IOSurface/IOSurface.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Resolve a window's backing IOSurface via the injected WindowServer helper.
// Returns a CFRetained IOSurfaceRef (+1 reference) on success, or NULL when the
// service is unreachable or the surface could not be resolved.
IOSurfaceRef rdgpu_macsurface_resolve_window(uint32_t window_id);

#ifdef __cplusplus
}
#endif

#endif // RDGPU_MACSURFACE_CLIENT_H
