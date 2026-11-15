// rdgpu_macsurface_client.m — shim-side client for the WindowServer-injected
// mac-surface resolver (com.rdgpu.macsurface).
//
// Sends rdgpu_resolve_msg_t {window_id} to the injected payload and receives
// rdgpu_resolve_reply_t carrying the window's backing IOSurface as a mach
// send-right, which is turned back into an IOSurfaceRef via
// IOSurfaceLookupFromMachPort. This is the "actual pixels" path: no
// CGWindowListCreateImage and no Screen Recording TCC.
//
// The wire layout is #included from the SAME rdgpu_protocol.h the payload uses,
// so the two sides cannot drift. Bootstrap lookup mirrors rdgpu_scanout_client.m
// (own domain first, then the gui/<uid> per-user domain).

#import <IOSurface/IOSurface.h>

#include <servers/bootstrap.h>
#include <mach/mach.h>
#include <mach/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "rdgpu_macsurface_client.h"
#include "rdgpu_protocol.h"

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

// Look up com.rdgpu.macsurface: own bootstrap domain first, then gui/<uid>.
static kern_return_t rdgpu_macsurface_lookup(mach_port_t *out) {
    *out = MACH_PORT_NULL;
    mach_port_t bp = MACH_PORT_NULL;
    if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS || bp == MACH_PORT_NULL)
        return KERN_FAILURE;

    kern_return_t kr = bootstrap_look_up(bp, RDGPU_MACSURFACE_SERVICE_NAME, out);
    if (kr == BOOTSTRAP_SUCCESS && *out != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), bp);
        return KERN_SUCCESS;
    }
    fn_look_up_per_user per_user = rdgpu_look_up_per_user();
    if (per_user) {
        kern_return_t kr2 = per_user(bp, RDGPU_MACSURFACE_SERVICE_NAME, getuid(), out);
        if (kr2 == BOOTSTRAP_SUCCESS && *out != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), bp);
            return KERN_SUCCESS;
        }
        kr = kr2;
    }
    mach_port_deallocate(mach_task_self(), bp);
    return kr;
}

// Resolve a window's backing IOSurface via the injected helper. Returns a
// CFRetained IOSurfaceRef (+1) or NULL. Logs a marker line per distinct failure
// so a service that is not yet injected does not spam once per attempt.
IOSurfaceRef rdgpu_macsurface_resolve_window(uint32_t window_id) {
    if (!window_id) return NULL;

    static kern_return_t last_kr = INT_MIN;
    mach_port_t service = MACH_PORT_NULL;
    kern_return_t kr = rdgpu_macsurface_lookup(&service);
    if (kr != KERN_SUCCESS || service == MACH_PORT_NULL) {
        if (kr != last_kr) {
            last_kr = kr;
            fprintf(stderr, "RDGPU_MACSURFACE_LOOKUP service=%s kr=%d\n",
                    RDGPU_MACSURFACE_SERVICE_NAME, kr);
        }
        return NULL;
    }
    last_kr = INT_MIN;

    mach_port_t reply_port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply_port) != KERN_SUCCESS)
        reply_port = MACH_PORT_NULL;

    rdgpu_resolve_msg_t msg;
    memset(&msg, 0, sizeof msg);
    msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
        reply_port != MACH_PORT_NULL ? MACH_MSG_TYPE_MAKE_SEND_ONCE : 0);
    msg.header.msgh_size = sizeof msg;
    msg.header.msgh_remote_port = service;
    msg.header.msgh_local_port = reply_port;
    msg.header.msgh_id = RDGPU_MSG_ID_RESOLVE;
    msg.body.msgh_descriptor_count = 0;
    msg.window_id = window_id;

    kr = mach_msg(&msg.header, MACH_SEND_MSG, sizeof msg, 0,
                  MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    mach_port_deallocate(mach_task_self(), service);
    if (kr != KERN_SUCCESS || reply_port == MACH_PORT_NULL) {
        if (reply_port != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), reply_port);
        return NULL;
    }

    int timeout_ms = 1000;
    const char *e = getenv("RDGPU_MACSURFACE_TIMEOUT_MS");
    if (e && *e) timeout_ms = atoi(e);

    union {
        rdgpu_resolve_reply_t reply;
        uint8_t raw[256];
    } rbuf;
    memset(&rbuf, 0, sizeof rbuf);
    kr = mach_msg((mach_msg_header_t *)&rbuf, MACH_RCV_MSG, 0, sizeof rbuf, reply_port,
                  (mach_msg_timeout_t)timeout_ms, MACH_PORT_NULL);
    mach_port_deallocate(mach_task_self(), reply_port);
    if (kr != KERN_SUCCESS) return NULL;

    rdgpu_resolve_reply_t *rp = &rbuf.reply;
    if (rp->result != 0 || rp->body.msgh_descriptor_count < 1 ||
        rp->surface_port.type != MACH_MSG_PORT_DESCRIPTOR ||
        rp->surface_port.name == MACH_PORT_NULL) {
        if (rp->surface_port.name != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), rp->surface_port.name);
        return NULL;
    }

    mach_port_t sp = rp->surface_port.name;
    IOSurfaceRef io = IOSurfaceLookupFromMachPort(sp);
    mach_port_deallocate(mach_task_self(), sp);
    if (io) {
        fprintf(stderr, "RDGPU_MACSURFACE_RESOLVED window=%u iosurface_id=%u %zux%zu\n",
                window_id, IOSurfaceGetID(io), IOSurfaceGetWidth(io), IOSurfaceGetHeight(io));
    }
    return io;   /* +1 reference */
}
