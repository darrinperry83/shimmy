// rdgpu_protocol.h — shared wire protocol between the rdgpu-scanout.osax payload (loaded into
// the Dock) and the shim client that sends an IOSurface to present.
//
// This header is included by both payload.m and any client/tester so the message layout and the
// service name can never drift.

#ifndef RDGPU_PROTOCOL_H
#define RDGPU_PROTOCOL_H

#include <mach/mach.h>

#define RDGPU_SERVICE_NAME   "com.rdgpu.scanout"
#define RDGPU_MSG_ID_PRESENT 0x52445001u   // 'RDP\x01'

// ---------------------------------------------------------------------------
// Window → IOSurface resolver (mac-app window content without screen capture).
// The WindowServer-injected payload receives RDGPU_MSG_ID_RESOLVE carrying a
// CGWindowID and replies with the window's backing IOSurface as a mach send-right
// (IOSurfaceCreateMachPort). The shim turns that back into an IOSurfaceRef with
// IOSurfaceLookupFromMachPort. This is the "actual pixels" path; no
// CGWindowListCreateImage and no Screen Recording TCC.
#define RDGPU_MACSURFACE_SERVICE_NAME "com.rdgpu.macsurface"
#define RDGPU_MSG_ID_RESOLVE 0x52445002u   // 'RDP\x02'

// One "resolve this window's IOSurface" request (shim -> payload).
typedef struct {
    mach_msg_header_t header;      // msgh_local_port = reply port (MAKE_SEND_ONCE)
    mach_msg_body_t   body;        // descriptor_count = 0
    uint32_t          window_id;   // CGWindowID
    uint32_t          reserved;
} rdgpu_resolve_msg_t;

// The reply (payload -> shim). Carries the resolved IOSurface as a COPY_SEND
// port descriptor (or MACH_PORT_NULL / result != 0 when it could not resolve).
typedef struct {
    mach_msg_header_t          header;
    mach_msg_body_t            body;
    mach_msg_port_descriptor_t surface_port;   // IOSurface send-right (COPY_SEND)
    uint32_t                   surface_id;     // IOSurfaceGetID (0 = none)
    int32_t                    result;         // 0 = resolved, else error code
} rdgpu_resolve_reply_t;

// Per-display ROLE carried in the present message's display_index field (single source
// of truth shared by the shim client and the payload). The payload keeps one handoff slot
// + one patched SLCADisplay per role and maps the role to a SkyLight display id.
#define RDGPU_SCANOUT_ROLE_PRIMARY   0u   // primary output   = WAYLAND-1 / external
#define RDGPU_SCANOUT_ROLE_SECONDARY 1u   // secondary output = WAYLAND-2 / builtin

// One "present this IOSurface" request.
//   header        — msgh_bits carries MACH_MSGH_BITS_COMPLEX plus MACH_MSG_TYPE_COPY_SEND on the
//                   surface port; msgh_remote_port = the service send-right; msgh_local_port =
//                   MACH_PORT_NULL for one-way, or a reply port if a reply is wanted.
//   surface_port  — the IOSurface send-right from IOSurfaceCreateMachPort().
//   surface_id    — informational (IOSurfaceGetID); used for logging and as a fallback if the
//                   caller cannot transfer the port (then IOSurfaceLookup(id) is used instead).
//   display_index — per-display ROLE, not a raw CGDirectDisplayID:
//                       0 = primary output   (WAYLAND-1 / external)
//                       1 = secondary output (WAYLAND-2 / builtin)
//                   The payload keeps one handoff slot + one patched SLCADisplay per role and
//                   maps role 0 -> the SkyLight display id in /tmp/rdgpu-scanout-display
//                   (default 0x1e) and role 1 -> /tmp/rdgpu-scanout-display2 (default 0x27).
//                   Any other value is treated as role 0.
typedef struct {
    mach_msg_header_t          header;
    mach_msg_body_t            body;
    mach_msg_port_descriptor_t surface_port;
    uint32_t                   surface_id;
    uint32_t                   display_index;
} rdgpu_present_msg_t;

// Optional one-word reply (sent only if the caller supplied a reply port).
typedef struct {
    mach_msg_header_t header;
    int32_t           result;   // return code of CoreDisplay_Display_PresentIOSurface
} rdgpu_reply_t;

#endif // RDGPU_PROTOCOL_H
