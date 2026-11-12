// rdgpu_scanout_client.m — shim-side sender for the Dock's com.rdgpu.scanout service.
//
// The Dock loads rdgpu-scanout.osax; its payload registers "com.rdgpu.scanout" in the Dock's
// Aqua gui/<uid> bootstrap domain and, on receiving an IOSurface mach send-right, presents it
// via the private CoreDisplay_Display_PresentIOSurface (WindowServer-free scanout).
//
// This file is the client half. It is the mirror image of rdgpu-scanout-osax/payload.m's receive
// path and rdgpu-scanout-osax/self_test.m's send path. The message layout is #included from the
// SAME rdgpu_protocol.h the payload uses, so the two sides cannot drift.
//
// BOOTSTRAP DOMAIN. The payload registers with bootstrap_register() on the Dock's own bootstrap
// port, which for a GUI app is the Aqua gui/<uid> domain. A client therefore has to look the
// service up in *that* domain, not necessarily its own:
//   - If the shim is itself a normal GUI app / launched from Terminal in the same session, its
//     task bootstrap port already IS the gui/<uid> domain and bootstrap_look_up() just works.
//   - If the shim runs under a harness that swapped its bootstrap port (a "harness" domain), the
//     first look-up returns BOOTSTRAP_UNKNOWN_SERVICE (1102); we then retry through
//     bootstrap_look_up_per_user(target_user = getuid()), which crosses into the current user's
//     gui/<uid> domain. That private launchd symbol is re-exported by libSystem (see
//     /usr/lib/system/liblaunch.tbd) and is dlsym-resolved here so a future system that drops it
//     degrades to the plain look-up instead of failing to link.
//
// GATING. Everything is OFF unless RDGPU_SCANOUT=1. The caller (window.m) checks
// rdgpu_scanout_enabled() and only then calls rdgpu_scanout_present().

#import <IOSurface/IOSurface.h>
#import <CoreGraphics/CoreGraphics.h>

#include <servers/bootstrap.h>
#include <mach/mach.h>
#include <mach/message.h>
#include <mach/task_special_ports.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "rdgpu_scanout_client.h"
// The canonical shared wire protocol lives in the OSAX tree (sibling of shim-metal-clear) so
// this client and rdgpu-scanout-osax/payload.m compile the SAME struct layout; a relative
// include keeps a single source of truth rather than a drifting copy.
#include "../../rdgpu-scanout-osax/rdgpu_protocol.h"

// ---------------------------------------------------------------------------------------------
// The gate.

int rdgpu_scanout_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RDGPU_SCANOUT");
        v = (e && *e && *e != '0');
        if (v) {
            static int said;
            if (!said) {
                said = 1;
                fprintf(stderr, "RDGPU_SCANOUT=1: direct scanout to %s is enabled; frames will "
                                "be sent to the Dock OSAX instead of the CAMetalLayer when the "
                                "service is reachable.\n", RDGPU_SERVICE_NAME);
            }
        }
    }
    return v;
}

// ---------------------------------------------------------------------------------------------
// The private launchd cross-user look-up (gui/<uid> domain). Not in the public SDK; the symbol is
// stable and re-exported through libSystem, but we dlsym it so a missing symbol cannot break the
// link or the process.
typedef kern_return_t (*fn_look_up_per_user)(mach_port_t bp, const name_t service_name,
                                             uid_t target_user, mach_port_t *server_port);

static fn_look_up_per_user rdgpu_look_up_per_user(void) {
    static fn_look_up_per_user fn;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        fn = (fn_look_up_per_user)dlsym(RTLD_DEFAULT, "bootstrap_look_up_per_user");
    }
    return fn;
}

// ---------------------------------------------------------------------------------------------
// Diagnostics. The RDGPU_SCANOUT_LOOKUP marker is printed for the first attempt and then again
// whenever the kernel return / domain changes, so a service that stays down logs once rather than
// once per frame.
static void rdgpu_log_lookup(const char* domain, kern_return_t kr) {
    static kern_return_t last_kr = INT_MIN;
    static char last_domain[64];
    if (kr == last_kr && last_domain[0] && strcmp(last_domain, domain) == 0) return;
    last_kr = kr;
    strlcpy(last_domain, domain, sizeof last_domain);
    fprintf(stderr, "RDGPU_SCANOUT_LOOKUP service=%s domain=%s kr=%d\n",
            RDGPU_SERVICE_NAME, domain, kr);
}

// Look the service up: own bootstrap domain first, then the current user's gui/<uid> domain.
// Returns KERN_SUCCESS and writes the send-right to *out, or the last kernel return.
static kern_return_t rdgpu_scanout_lookup(mach_port_t* out) {
    *out = MACH_PORT_NULL;

    mach_port_t bp = MACH_PORT_NULL;
    if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS || bp == MACH_PORT_NULL) {
        fprintf(stderr, "RDGPU_SCANOUT_LOOKUP service=%s domain=none kr=no_bootstrap_port\n",
                RDGPU_SERVICE_NAME);
        return KERN_FAILURE;
    }

    kern_return_t kr = bootstrap_look_up(bp, RDGPU_SERVICE_NAME, out);
    rdgpu_log_lookup("self", kr);
    if (kr == BOOTSTRAP_SUCCESS && *out != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), bp);
        return KERN_SUCCESS;
    }

    fn_look_up_per_user per_user = rdgpu_look_up_per_user();
    if (per_user) {
        char domain[64];
        snprintf(domain, sizeof domain, "gui/%d", (int)getuid());
        kern_return_t kr2 = per_user(bp, RDGPU_SERVICE_NAME, getuid(), out);
        rdgpu_log_lookup(domain, kr2);
        if (kr2 == BOOTSTRAP_SUCCESS && *out != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), bp);
            return KERN_SUCCESS;
        }
        kr = kr2;
    }

    mach_port_deallocate(mach_task_self(), bp);
    return kr;
}

// ---------------------------------------------------------------------------------------------
// The sender.

int rdgpu_scanout_present(IOSurfaceRef surface, CGDirectDisplayID display) {
    if (!surface) return -1;

    // The mach send-right the payload receives and turns back into the IOSurface via
    // IOSurfaceLookupFromMachPort().
    mach_port_t surface_port = IOSurfaceCreateMachPort(surface);
    if (surface_port == MACH_PORT_NULL) return -2;

    mach_port_t service = MACH_PORT_NULL;
    kern_return_t kr = rdgpu_scanout_lookup(&service);
    if (kr != KERN_SUCCESS || service == MACH_PORT_NULL) {
        if (surface_port != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), surface_port);
        return -3;
    }

    // Reply port so we can learn the Dock's CoreDisplay_Display_PresentIOSurface result.
    mach_port_t reply_port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply_port) != KERN_SUCCESS) {
        reply_port = MACH_PORT_NULL;             // degrade to one-way below
    }

    rdgpu_present_msg_t msg;
    memset(&msg, 0, sizeof msg);
    // msgh_size MUST be set: memset leaves it 0 and the kernel rejects/truncates a
    // header with a 0 size (MACH_SEND_INVALID_HEADER). Without it the body — and
    // therefore the surface_port COPY_SEND descriptor — is not transmitted, so the
    // server receives a descriptor_count==0 / surface_id==0 message and its
    // IOSurfaceLookupFromMachPort() gets nothing (reason=no_surface).
    msg.header.msgh_size = sizeof msg;
    // A non-null reply port MUST be sent as MAKE_SEND_ONCE (the kernel turns it into
    // a send-once reply right for the server); leaving the local bits at 0
    // (MACH_MSG_TYPE_UNSTRUCTURED) with a live msgh_local_port fails the send with
    // MACH_SEND_INVALID_HEADER. One-way (NULL reply port) keeps local bits 0.
    msg.header.msgh_bits = MACH_MSGH_BITS_COMPLEX |
        MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                       reply_port != MACH_PORT_NULL ? MACH_MSG_TYPE_MAKE_SEND_ONCE : 0);
    msg.header.msgh_remote_port = service;
    msg.header.msgh_local_port  = reply_port;    // MACH_PORT_NULL == one-way
    msg.header.msgh_id          = RDGPU_MSG_ID_PRESENT;
    msg.body.msgh_descriptor_count = 1;
    msg.surface_port.name        = surface_port;
    msg.surface_port.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg.surface_port.type        = MACH_MSG_PORT_DESCRIPTOR;
    msg.surface_id               = (uint32_t)IOSurfaceGetID(surface);
    // display_index carries the per-display ROLE (0 = primary / WAYLAND-1, 1 = secondary /
    // WAYLAND-2); the payload maps it to a SkyLight display id via /tmp/rdgpu-scanout-display
    // (role 0) and /tmp/rdgpu-scanout-display2 (role 1).
    msg.display_index            = (uint32_t)display;

    kr = mach_msg(&msg.header, MACH_SEND_MSG, sizeof msg, 0,
                  MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    // COPY_SEND: we still hold our copy of the surface send-right; release it.
    mach_port_deallocate(mach_task_self(), surface_port);
    mach_port_deallocate(mach_task_self(), service);

    if (kr != KERN_SUCCESS) {
        if (reply_port != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), reply_port);
        return -4;
    }

    if (reply_port == MACH_PORT_NULL) return 0;   // one-way: nothing more to learn

    int timeout_ms = 1000;
    const char* e = getenv("RDGPU_SCANOUT_TIMEOUT_MS");
    if (e && *e) timeout_ms = atoi(e);

    rdgpu_reply_t reply;
    memset(&reply, 0, sizeof reply);
    // Receive into a buffer large enough for the reply + the kernel's message trailer;
    // sizeof(reply) alone (28) is too small and fails with MACH_RCV_TOO_LARGE.
    union {
        rdgpu_reply_t reply;
        uint8_t       raw[128];
    } rbuf;
    memset(&rbuf, 0, sizeof rbuf);
    kr = mach_msg((mach_msg_header_t*)&rbuf, MACH_RCV_MSG, 0, sizeof rbuf, reply_port,
                  (mach_msg_timeout_t)timeout_ms, MACH_PORT_NULL);
    mach_port_deallocate(mach_task_self(), reply_port);
    if (kr != KERN_SUCCESS) return -5;            // e.g. MACH_RCV_TIMED_OUT

    reply = rbuf.reply;
    int32_t result = reply.result;
    // The server moved a send-once reply right to us; the received msgh_remote_port is
    // MACH_PORT_NULL here (no reply-to), but guard anyway in case a future server echoes one.
    if (reply.header.msgh_remote_port != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), reply.header.msgh_remote_port);
    return (int)result;                           // the Dock's CoreDisplay present return code
}
