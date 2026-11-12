// rdgpu_scanout_client.h — shim-side sender for the Dock's direct-scanout Mach service.
//
// The Dock loads rdgpu-scanout.osax, whose payload registers the bootstrap service
// "com.rdgpu.scanout" in the Dock's Aqua gui/<uid> domain and, on receiving an IOSurface
// mach send-right, presents it straight to the display via the private
// CoreDisplay_Display_PresentIOSurface — WindowServer-free scanout.
//
// This is the client half. The wire layout is shared with the payload through
// rdgpu_protocol.h so the two sides can never drift (see rdgpu_scanout_client.m).
//
// Everything here is OFF unless RDGPU_SCANOUT=1 is set; the Metal/CAMetalLayer present path in
// window.m stays untouched otherwise.

#ifndef RDGPU_SCANOUT_CLIENT_H
#define RDGPU_SCANOUT_CLIENT_H

#include <IOSurface/IOSurface.h>
#include <CoreGraphics/CoreGraphics.h>

// Per-display ROLE carried in the present message's display_index field. Defined once in
// rdgpu_protocol.h (shared with the payload): 0 = primary / WAYLAND-1, 1 = secondary /
// WAYLAND-2. The server maps role 0 -> /tmp/rdgpu-scanout-display (default 0x1e, external)
// and role 1 -> /tmp/rdgpu-scanout-display2 (default 0x27, builtin).
#include "../../rdgpu-scanout-osax/rdgpu_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// Non-zero when the RDGPU_SCANOUT=1 gate is set (anything other than unset/"0"). Read once.
int rdgpu_scanout_enabled(void);

// Send `surface` to the Dock's com.rdgpu.scanout service for direct scanout.
//
// `display` is a per-display ROLE (RDGPU_SCANOUT_ROLE_PRIMARY for the primary output /
// WAYLAND-1, RDGPU_SCANOUT_ROLE_SECONDARY for the secondary output / WAYLAND-2). It is carried
// verbatim as the message's display_index; the server maps the role to a SkyLight display id.
//
// Does: IOSurfaceCreateMachPort -> bootstrap_look_up (own domain, then the gui/<uid> per-user
// domain) -> mach_msg carrying the surface send-right -> waits for the one-word reply.
//
// Returns, on a complete round-trip, the Dock's CoreDisplay_Display_PresentIOSurface result
// (0 == present accepted). On a transport failure it returns a negative code:
//    -1  surface == NULL
//    -2  IOSurfaceCreateMachPort failed
//    -3  service not reachable in any bootstrap domain (see RDGPU_SCANOUT_LOOKUP log line)
//    -4  mach_msg send failed
//    -5  no reply within the timeout (RDGPU_SCANOUT_TIMEOUT_MS, default 1000)
int rdgpu_scanout_present(IOSurfaceRef surface, CGDirectDisplayID display);

#ifdef __cplusplus
}
#endif

#endif // RDGPU_SCANOUT_CLIENT_H
