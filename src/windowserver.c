// windowserver.c — shim-as-WindowServer stand-in (first cut): Mach bootstrap check-in +
// a MIG-style CGS/SLS server loop answering the ~37-function background-service surface.
//
// See windowserver.h for the protocol and the "real vs stubbed" split. This file is the
// server half; ws_test_peer.c is the validation peer.
//
// Safety: this process NEVER registers the real `com.apple.windowserver` name unless it is
// run as root with RDNATIVE_WINDOWSERVER_SYSTEM=1 (which the validation run does not do).
// The default mode registers a *test* name via bootstrap_register, so the check-in and the
// handshake can be proven headlessly without a launchd plist and without disturbing the
// live WindowServer.
#include "windowserver.h"

#include <mach/mach.h>
#include <mach/message.h>
#include <mach/task_special_ports.h>
#include <mach/mach_port.h>
#include <servers/bootstrap.h>

#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// MIG's reply-bit helper (defined by the generated stubs; not in the public headers).
#define MACH_MSGH_BITS_REPLY(bits) MACH_MSGH_BITS_REMOTE(bits)

// ---- server state ---------------------------------------------------------------------
typedef struct ws_conn {
    uint32_t cid;
    mach_port_t port;        // receive right == send-right name for the per-connection port
    pid_t pid;
    uint32_t version;
    mach_port_t notify_port; // last registered notification port (send right)
    mach_port_t event_port;  // receive right for the event stream (== its send-right name)
    struct ws_conn *next;
} ws_conn_t;

typedef struct ws_window {
    uint32_t wid;
    uint32_t cid;
    ws_rect_t rect;
    int32_t level;
    uint64_t tags;
    uint32_t surface_id;     // bound IOSurface id (IOSurfaceGetID); 0 = none bound
    int      has_surface;    // 1 once SLSBindSurface attached a surface
    struct ws_window *next;
} ws_window_t;

#define WS_MAX_PROPS 16
typedef struct ws_prop {
    uint32_t cid;
    char key[WS_PROP_KEY_MAX];
    uint32_t vlen;
    uint8_t val[WS_PROP_VAL_MAX];
} ws_prop_t;

static mach_port_t g_main_port = MACH_PORT_NULL;   // receive right of the main service
static mach_port_t g_main_send = MACH_PORT_NULL;   // send right to self (SLSServerPort)
static mach_port_t g_active_port = MACH_PORT_NULL;
static mach_port_t g_port_set = MACH_PORT_NULL;    // port set: main + every per-connection port

static ws_conn_t *g_conns = NULL;
static ws_window_t *g_windows = NULL;
static ws_prop_t g_props[WS_MAX_PROPS];
static uint32_t g_next_cid = WS_FIRST_MINTED_CID;
static uint32_t g_next_wid = WS_FIRST_WINDOW_ID;
static uint32_t g_next_tid = WS_FIRST_TRANSACTION_ID;
static uint32_t g_loginwindow_conn = 0;
static char g_login_name[WS_LOGIN_NAME_MAX];
static uint32_t g_session_id = 0x186a0;            // arbitrary but stable login session id
static volatile sig_atomic_t g_stop = 0;
static int g_verbose = 1;

// ---- small helpers --------------------------------------------------------------------
static pid_t ws_audit_pid(const audit_token_t *tok) { return (pid_t)tok->val[5]; }
static uint32_t ws_audit_pidversion(const audit_token_t *tok) { return tok->val[7]; }

static ws_conn_t *ws_find_conn(uint32_t cid) {
    for (ws_conn_t *c = g_conns; c; c = c->next) if (c->cid == cid) return c;
    return NULL;
}

static ws_window_t *ws_find_window(uint32_t wid) {
    for (ws_window_t *w = g_windows; w; w = w->next) if (w->wid == wid) return w;
    return NULL;
}

static void ws_log(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

// Weak surfacing handoff: the windowserver stand-in alone cannot create a wl_surface (that
// lives in the surfacing host, wlsrv.c mac_surface_*). When the two are linked into one image
// the STRONG ws_mac_surface_present there overrides this; standalone, we log the would-be
// handoff — which is exactly what the headless SLSBindSurface wiring test asserts against.
__attribute__((weak)) void ws_mac_surface_present(uint32_t window_id, uint32_t surface_id) {
    ws_log("[ws-surface] SLSBindSurface window=%u iosurface=%u -> surfacing handoff "
           "(headless): would create wl_surface + xdg_toplevel in Hyprland\n",
           window_id, surface_id);
}

// Allocate a receive right and give the server a send right to it (same name), so it can be
// handed back to a client in a complex reply with COPY_SEND.
static mach_port_t ws_alloc_port(void) {
    mach_port_t recv = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &recv) != KERN_SUCCESS)
        return MACH_PORT_NULL;
    mach_port_insert_right(mach_task_self(), recv, recv, MACH_MSG_TYPE_MAKE_SEND);
    return recv;
}

static void ws_free_port(mach_port_t p) {
    if (p == MACH_PORT_NULL) return;
    mach_port_deallocate(mach_task_self(), p);                          // drop the send right
    mach_port_mod_refs(mach_task_self(), p, MACH_PORT_RIGHT_RECEIVE, -1); // drop the receive right
}

// Extract the audit token from a received message's trailer.
static audit_token_t ws_msg_audit_token(const mach_msg_header_t *msg) {
    mach_msg_audit_trailer_t *t =
        (mach_msg_audit_trailer_t *)((uintptr_t)msg + round_msg(msg->msgh_size));
    return t->msgh_audit;
}

// ---- pre-held service handoff (chooser -> stand-in) -----------------------------------
// The boot chooser claims `com.apple.windowserver` + `.active` IMMEDIATELY at startup (before
// its countdown), so the real WindowServer loses the race and exits. fork+exec do NOT carry Mach
// port rights between tasks on this platform, so the stand-in does not re-check-in (which would
// re-open the race). Instead it rendezvous with the chooser through the `.active` service (which
// no client sends SLS traffic to): look it up, send back a reply port, and receive BOTH receive
// rights as MACH_MSG_TYPE_MOVE_RECEIVE. The stand-in then owns the service with no second
// check-in.
#define SHIMMY_SERVICE_PREHELD_ENV      "SHIMMY_SERVICE_PREHELD"
#define SHIMMY_SERVICE_MAIN_ENV         "SHIMMY_SERVICE_MAIN_NAME"
#define SHIMMY_SERVICE_ACTIVE_ENV       "SHIMMY_SERVICE_ACTIVE_NAME"

#define SHIMMY_HANDOFF_REQ_ID           0x57534831u  // "WSH1": stand-in -> chooser (reply port)
#define SHIMMY_HANDOFF_GRANT_ID         0x57534832u  // "WSH2": chooser -> stand-in (2 receive rights)

// Wire structs MUST match byte-for-byte between windowserver.c and the chooser (chooser.c).
typedef struct {
    mach_msg_header_t          hdr;
    mach_msg_body_t            body;
    mach_msg_port_descriptor_t pd;      // reply port C (COPY_SEND)
} shimmy_handoff_req_t;

typedef struct {
    mach_msg_header_t          hdr;
    mach_msg_body_t            body;
    mach_msg_port_descriptor_t pd_main;    // main service (MOVE_RECEIVE)
    mach_msg_port_descriptor_t pd_active;  // active service (MOVE_RECEIVE)
} shimmy_handoff_grant_t;

// Receive the ALREADY-CLAIMED service rights from the chooser over the `.active` rendezvous.
static kern_return_t ws_bootstrap_receive_preheld(mach_port_t bp,
                                                  const char* active_name,
                                                  mach_port_t* main_out,
                                                  mach_port_t* active_out) {
    mach_port_t active_send = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_look_up(bp, (char*)active_name, &active_send);
    if (kr != KERN_SUCCESS) return kr;

    mach_port_t reply = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply) != KERN_SUCCESS)
        return KERN_RESOURCE_SHORTAGE;
    mach_port_insert_right(mach_task_self(), reply, reply, MACH_MSG_TYPE_MAKE_SEND);

    shimmy_handoff_req_t req;
    memset(&req, 0, sizeof req);
    req.hdr.msgh_bits = MACH_MSGH_BITS_COMPLEX |
                        MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    req.hdr.msgh_remote_port = active_send;
    req.hdr.msgh_id          = SHIMMY_HANDOFF_REQ_ID;
    req.hdr.msgh_size        = sizeof req;
    req.body.msgh_descriptor_count = 1;
    req.pd.name              = reply;
    req.pd.disposition       = MACH_MSG_TYPE_COPY_SEND;
    req.pd.type              = MACH_MSG_PORT_DESCRIPTOR;
    kr = mach_msg(&req.hdr, MACH_SEND_MSG, sizeof req, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) { ws_free_port(reply); return kr; }

    uint8_t rbuf[512];
    memset(rbuf, 0, sizeof rbuf);
    kr = mach_msg((mach_msg_header_t*)rbuf, MACH_RCV_MSG | MACH_RCV_LARGE, 0, sizeof rbuf,
                  reply, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) { ws_free_port(reply); return kr; }

    shimmy_handoff_grant_t* g = (shimmy_handoff_grant_t*)rbuf;
    if (g->hdr.msgh_id != SHIMMY_HANDOFF_GRANT_ID || g->body.msgh_descriptor_count < 2) {
        ws_free_port(reply);
        return KERN_FAILURE;
    }
    *main_out   = g->pd_main.name;
    *active_out = g->pd_active.name;
    ws_free_port(reply);   // done with the rendezvous reply port
    return KERN_SUCCESS;
}

// ---- bootstrap check-in ---------------------------------------------------------------
// The REAL path, mirroring WINDOWSERVER_BOOTSTRAP_REGISTRATION.md §3.2 exactly:
// bootstrap_check_in("com.apple.windowserver") then bootstrap_check_in("com.apple.windowserver.active").
// Only meaningful when launched by launchd from a plist that declares those (HideUntilCheckIn)
// names. Never used by the validation run.
kern_return_t ws_bootstrap_check_in_system(mach_port_t bp,
                                           mach_port_t *main_out,
                                           mach_port_t *active_out) {
    name_t main_name, active_name;
    strlcpy(main_name, WS_REAL_SERVICE_NAME, sizeof main_name);
    strlcpy(active_name, WS_REAL_ACTIVE_NAME, sizeof active_name);

    kern_return_t kr = bootstrap_check_in(bp, main_name, main_out);
    if (kr != KERN_SUCCESS) return kr;
    // Order is significant: main first, .active second (the report pins this).
    kr = bootstrap_check_in(bp, active_name, active_out);
    if (kr != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), *main_out);
        *main_out = MACH_PORT_NULL;
    }
    return kr;
}

// The SAFE test path: allocate a receive right and bootstrap_register it under a test name.
// bootstrap_register passes the port as a COPY_SEND, so we first insert a send right to self.
kern_return_t ws_bootstrap_register_test(mach_port_t bp,
                                         const char *service_name,
                                         const char *active_name,
                                         mach_port_t *main_out,
                                         mach_port_t *active_out) {
    name_t n;
    mach_port_t recv = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &recv) != KERN_SUCCESS)
        return KERN_RESOURCE_SHORTAGE;
    mach_port_insert_right(mach_task_self(), recv, recv, MACH_MSG_TYPE_MAKE_SEND);

    strlcpy(n, service_name, sizeof n);
    kern_return_t kr = bootstrap_register(bp, n, recv);
    if (kr != KERN_SUCCESS) {
        ws_free_port(recv);
        return kr;
    }
    *main_out = recv;

    // The .active service mirrors the report order: main first, .active second.
    recv = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &recv) != KERN_SUCCESS) {
        ws_free_port(*main_out);
        *main_out = MACH_PORT_NULL;
        return KERN_RESOURCE_SHORTAGE;
    }
    mach_port_insert_right(mach_task_self(), recv, recv, MACH_MSG_TYPE_MAKE_SEND);
    strlcpy(n, active_name, sizeof n);
    kr = bootstrap_register(bp, n, recv);
    if (kr != KERN_SUCCESS) {
        ws_free_port(recv);
        ws_free_port(*main_out);
        *main_out = MACH_PORT_NULL;
        return kr;
    }
    *active_out = recv;
    return KERN_SUCCESS;
}

// ---- routine names (indexed by routine id - WS_R_FIRST; ids are contiguous 1000..1036) ----
const char *ws_routine_name(uint32_t id) {
    static const char *const names[] = {
        "SLSMainConnectionID", "SLSNewConnection", "SLSServerPort", "SLSReleaseConnection",
        "SLSConnectionGetPIDAndVersion", "SLSGetConnectionIDForPSN",
        "SLSSessionCopyCurrentSessionProperties", "SLSSessionCopyAllSessionProperties",
        "SLSSessionSetCurrentSessionProperties", "SLSSetLoginwindowConnection", "SLSSetLoginName",
        "SLSSessionSwitchToSessionID", "SLSSetConnectionProperty", "SLSCopyConnectionProperty",
        "SLSSessionSetCurrentSessionWorkspaceNotificationConnectionID",
        "SLSSessionSetCurrentSessionWorkspacePreferences",
        "SLSRegisterConnectionNotifyProc", "SLSRemoveConnectionNotifyProc",
        "SLSRegisterNotifyProc", "SLSRemoveNotifyProc",
        "SLSGetEventPort",
        "SLSMainDisplayID", "SLSGetDisplayList", "SLSGetDisplayBounds", "SLSGetDisplayForUUID",
        "SLSCopyDisplayUUID", "SLSGetCurrentDisplayMode", "SLSGetPotentiallyActiveDisplayList",
        "SLSCopyActiveMenuBarDisplayIdentifier",
        "SLSNewWindowWithOpaqueShape", "SLSNewWindowWithOpaqueShapeAndContext", "SLSOrderWindow",
        "SLSSetWindowLevel", "SLSSetWindowTags", "SLSReleaseWindow",
        "SLSTransactionCreate", "SLSTransactionCommit", "SLSBindSurface",
    };
    // The coverage table must stay in lockstep with WS_R_COUNT (38); a missing/extra name is a
    // compile error, not a silent NULL lookup.
    _Static_assert(sizeof names / sizeof names[0] == WS_R_COUNT,
                   "ws_routine_name table must cover all WS_R_* routines");
    if (id < WS_R_FIRST || id > WS_R_LAST) return "<unknown>";
    return names[id - WS_R_FIRST];
}

// ---- reply senders --------------------------------------------------------------------
static void ws_send_simple(const mach_msg_header_t *req, mach_msg_header_t *rep,
                           mach_msg_size_t size) {
    rep->msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REPLY(req->msgh_bits), 0);
    rep->msgh_remote_port = req->msgh_remote_port;
    rep->msgh_local_port = MACH_PORT_NULL;
    rep->msgh_id = req->msgh_id + 100;
    rep->msgh_reserved = 0;
    // Mach message bodies must be natural-word (4-byte) aligned; a variable-length reply like
    // SLSCopyConnectionProperty (32 + vlen) can otherwise land on a non-multiple-of-4 size and
    // mach_msg rejects it with MACH_SEND_MSG_TOO_SMALL. The reply structs are zeroed, so the pad
    // bytes are well-defined.
    size = (size + 3u) & ~3u;
    rep->msgh_size = size;
    kern_return_t kr = mach_msg(rep, MACH_SEND_MSG, size, 0, MACH_PORT_NULL,
                                MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) ws_log("[ws] send_simple(id %u, size %u) failed: 0x%x\n",
                                   req->msgh_id, size, kr);
}

// Send a reply that moves one send right (the connection/event/server port).
static void ws_send_port(const mach_msg_header_t *req, ws_reply_port *rep, mach_port_t port,
                         int32_t ret_code) {
    memset(rep, 0, sizeof *rep);
    rep->hdr.msgh_bits = MACH_MSGH_BITS_COMPLEX |
                         MACH_MSGH_BITS(MACH_MSGH_BITS_REPLY(req->msgh_bits), 0);
    rep->hdr.msgh_remote_port = req->msgh_remote_port;
    rep->hdr.msgh_local_port = MACH_PORT_NULL;
    rep->hdr.msgh_id = req->msgh_id + 100;
    rep->hdr.msgh_reserved = 0;
    rep->hdr.msgh_size = sizeof *rep;
    rep->body.msgh_descriptor_count = 1;
    rep->port.name = port;
    rep->port.pad1 = 0;
    rep->port.pad2 = 0;
    rep->port.disposition = MACH_MSG_TYPE_COPY_SEND;   // server keeps its send right
    rep->port.type = MACH_MSG_PORT_DESCRIPTOR;
    rep->ret_code = ret_code;
    mach_msg((mach_msg_header_t *)rep, MACH_SEND_MSG, sizeof *rep, 0, MACH_PORT_NULL,
             MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

// ---- REAL SkyLight SLS MIG reply sender ----------------------------------------------
// Sends a reply in the REAL wire format: msgh_id = request + 0x64, a MIG NDR_record at
// offset 0x18, then the ret_code + out-args the caller placed in the reply struct. The
// reply-port mechanics (MACH_MSGH_BITS_REPLY + the request's remote port) are identical
// to the stand-in protocol and were already validated end-to-end by ws_test_peer.
static void ws_send_real(const mach_msg_header_t *req, mach_msg_header_t *rep,
                         mach_msg_size_t size) {
    rep->msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REPLY(req->msgh_bits), 0);
    rep->msgh_remote_port = req->msgh_remote_port;
    rep->msgh_local_port = MACH_PORT_NULL;
    rep->msgh_voucher_port = MACH_PORT_NULL;
    rep->msgh_id = req->msgh_id + SLS_REPLY_OFFSET;
    rep->msgh_reserved = 0;
    // The NDR_record sits at offset sizeof(mach_msg_header_t) == 0x18 in every sls_reply_*.
    *(uint64_t *)((uint8_t *)rep + sizeof(mach_msg_header_t)) = SLS_NDR;
    size = (size + 3u) & ~3u;
    rep->msgh_size = size;
    kern_return_t kr = mach_msg(rep, MACH_SEND_MSG, size, 0, MACH_PORT_NULL,
                                MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) ws_log("[ws] send_real(id 0x%x) failed: 0x%x\n",
                                   req->msgh_id, kr);
}

// ---- REAL SkyLight SLS MIG dispatch ---------------------------------------------------
// The handshake + connection/window functions a real CoreGraphics/AppKit client calls over
// the 0x1513 / 0x80001513 envelope. Display *enumeration* (GetDisplayList/GetDisplayBounds/
// GetCurrentDisplayMode/MainDisplayID) is NOT an RPC on macOS 26.5.1 — SkyLight reads those
// from a local CoreDisplay cache (SkyLight+0x1b6574), so they are intentionally absent here.
static const char *ws_real_routine_name(uint32_t id) {
    switch (id) {
    case SLS_R_NewConnection: return "SLSNewConnection";
    case SLS_R_ConnectionGetPIDAndVersion: return "SLSConnectionGetPIDAndVersion";
    case SLS_R_NewWindowWithOpaqueShape: return "CreateWindow (NewWindow path)";
    case SLS_R_BindSurface: return "SLSBindSurface";
    case SLS_R_OrderWindow: return "SLSOrderWindow (single)";
    case SLS_R_OrderWindowList: return "SLSOrderWindowList";
    case SLS_R_GetWindowList: return "SLSGetWindowList";
    case SLS_R_ReleaseWindow: return "TerminateWindow (release)";
    case SLS_R_TransactionCommit: return "TransactionCommitSync";
    case SLS_R_TransactionCommitAsync: return "TransactionCommitAsync";
    case SLS_R_GetDisplayForUUID: return "SLSGetDisplayForUUID";
    default: return "<unknown-sls>";
    }
}

static void ws_dispatch_real(mach_msg_header_t *req, audit_token_t tok) {
    uint32_t id = req->msgh_id;
    ws_log("[ws] real %s (id 0x%x)\n", ws_real_routine_name(id), id);

    switch (id) {
    case SLS_R_NewConnection: {              // the 0x80001513 handshake (real)
        ws_conn_t *c = calloc(1, sizeof *c);
        sls_reply_newconn rep;
        memset(&rep, 0, sizeof rep);
        rep.hdr.msgh_bits = MACH_MSGH_BITS_COMPLEX |
                            MACH_MSGH_BITS(MACH_MSGH_BITS_REPLY(req->msgh_bits), 0);
        rep.hdr.msgh_remote_port = req->msgh_remote_port;
        rep.hdr.msgh_local_port = MACH_PORT_NULL;
        rep.hdr.msgh_voucher_port = MACH_PORT_NULL;
        rep.hdr.msgh_id = id + SLS_REPLY_OFFSET;
        rep.hdr.msgh_reserved = 0;
        rep.hdr.msgh_size = sizeof rep;
        rep.body.msgh_descriptor_count = 2;

        if (!c) {
            rep.port0.name = MACH_PORT_NULL;
            rep.port0.disposition = MACH_MSG_TYPE_MOVE_SEND;
            rep.port0.type = MACH_MSG_PORT_DESCRIPTOR;
            rep.port1 = rep.port0;
            rep.ndr = SLS_NDR;
            rep.cid = 0;
            mach_msg((mach_msg_header_t *)&rep, MACH_SEND_MSG, sizeof rep, 0,
                     MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
            break;
        }
        c->cid = g_next_cid++;
        c->port = ws_alloc_port();
        c->pid = ws_audit_pid(&tok);
        c->version = ws_audit_pidversion(&tok);
        c->next = g_conns;
        g_conns = c;
        // A real client speaks to this per-connection port from now on, so it must be in the
        // receive set alongside the main service port.
        if (g_port_set != MACH_PORT_NULL)
            mach_port_move_member(mach_task_self(), c->port, g_port_set);

        // desc[0] = the per-connection port (client uses it as msgh_remote_port from now on).
        rep.port0.name = c->port;
        rep.port0.pad1 = 0;
        rep.port0.pad2 = 0;
        rep.port0.disposition = MACH_MSG_TYPE_MOVE_SEND;
        rep.port0.type = MACH_MSG_PORT_DESCRIPTOR;
        // desc[1] = a second port (real SLSNewConnection hands back two send rights).
        rep.port1.name = ws_alloc_port();
        rep.port1.pad1 = 0;
        rep.port1.pad2 = 0;
        rep.port1.disposition = MACH_MSG_TYPE_MOVE_SEND;
        rep.port1.type = MACH_MSG_PORT_DESCRIPTOR;
        rep.ndr = SLS_NDR;
        rep.cid = c->cid;
        mach_msg((mach_msg_header_t *)&rep, MACH_SEND_MSG, sizeof rep, 0,
                 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        ws_log("[ws] real SLSNewConnection -> cid 0x%x port 0x%x\n", c->cid, c->port);
        break;
    }
    case SLS_R_ConnectionGetPIDAndVersion: {  // real: header+NDR+ret+pid+version
        sls_req_cid *r = (sls_req_cid *)req;
        sls_reply_pidver rep = {{0}, SLS_NDR, WS_ERR_INVALID_CONNECTION, 0, 0};
        ws_conn_t *c = ws_find_conn(r->cid);
        if (c) {
            rep.ret_code = WS_ERR_SUCCESS;
            rep.pid = (uint32_t)c->pid;
            rep.version = c->version;
        } else if (r->cid == WS_MAIN_CONNECTION_ID) {
            rep.ret_code = WS_ERR_SUCCESS;
            rep.pid = (uint32_t)ws_audit_pid(&tok);
            rep.version = WS_SERVER_VERSION;
        }
        ws_send_real(req, &rep.hdr, sizeof rep);
        break;
    }
    case SLS_R_GetWindowList: {              // real: header+NDR+ret+count+ids
        sls_reply_widlist rep;
        memset(&rep, 0, sizeof rep);
        rep.ndr = SLS_NDR;
        rep.ret_code = WS_ERR_SUCCESS;
        uint32_t n = 0;
        for (ws_window_t *w = g_windows; w && n < WS_MAX_WINDOWS; w = w->next)
            rep.wids[n++] = w->wid;
        rep.count = n;
        mach_msg_size_t sz = (mach_msg_size_t)(offsetof(sls_reply_widlist, wids) + n * 4);
        ws_send_real(req, &rep.hdr, sz);
        break;
    }
    case SLS_R_OrderWindowList:
    case SLS_R_OrderWindow: {                // real: accept + success (ordering no-op)
        sls_reply_ret rep = {{0}, SLS_NDR, WS_ERR_SUCCESS};
        ws_send_real(req, &rep.hdr, sizeof rep);
        break;
    }
    case SLS_R_NewWindowWithOpaqueShape: {   // real: CreateWindow (0x7637) -> mint a window id
        sls_req_createwindow *r = (sls_req_createwindow *)req;
        ws_window_t *w = calloc(1, sizeof *w);
        sls_reply_cid rep = {{0}, SLS_NDR, WS_ERR_FAILURE, 0};
        if (w) {
            w->wid = g_next_wid++;
            // The connection is the port this message arrived on, not a cid in the body;
            // the token is the opaque connection-context pointer (leave cid 0 for now).
            w->cid = 0;
            w->next = g_windows;
            g_windows = w;
            rep.ret_code = WS_ERR_SUCCESS;
            rep.cid = w->wid;
            ws_log("[ws] real CreateWindow op=0x%x -> window %u\n", r->op, w->wid);
        }
        ws_send_real(req, &rep.hdr, sizeof rep);
        break;
    }
    case SLS_R_BindSurface: {                // real: bind an IOSurface to a window -> surfacing
        sls_req_bindsurface *r = (sls_req_bindsurface *)req;
        ws_window_t *w = ws_find_window(r->wid);
        int32_t ret = WS_ERR_INVALID_WINDOW;
        if (w) {
            w->surface_id = r->surface_id;
            w->has_surface = 1;
            ret = WS_ERR_SUCCESS;
            ws_log("[ws] real SLSBindSurface window=%u surface=%u type=%u %dx%d\n",
                   r->wid, r->surface_id, r->type, r->w, r->h);
            ws_mac_surface_present(r->wid, r->surface_id);
        }
        sls_reply_ret rep = {{0}, SLS_NDR, ret};
        ws_send_real(req, &rep.hdr, sizeof rep);
        break;
    }
    case SLS_R_ReleaseWindow: {              // real: TerminateWindow (0x7535) -> destroy
        sls_req_terminatewindow *r = (sls_req_terminatewindow *)req;
        ws_window_t **pp = &g_windows;
        int32_t ret = WS_ERR_INVALID_WINDOW;
        while (*pp) {
            if ((*pp)->wid == r->wid) {
                ws_window_t *w = *pp;
                *pp = w->next;
                free(w);
                ret = WS_ERR_SUCCESS;
                break;
            }
            pp = &(*pp)->next;
        }
        ws_log("[ws] real TerminateWindow window=%u -> %d\n", r->wid, ret);
        sls_reply_ret rep = {{0}, SLS_NDR, ret};
        ws_send_real(req, &rep.hdr, sizeof rep);
        break;
    }
    case SLS_R_TransactionCommit:
    case SLS_R_TransactionCommitAsync: {     // real: commit is a no-op for the stand-in
        sls_reply_ret rep = {{0}, SLS_NDR, WS_ERR_SUCCESS};
        ws_send_real(req, &rep.hdr, sizeof rep);
        break;
    }
    default: {                               // any other real id -> benign success
        sls_reply_ret rep = {{0}, SLS_NDR, WS_ERR_SUCCESS};
        ws_send_real(req, &rep.hdr, sizeof rep);
        break;
    }
    }
}

// ---- the ~37 handlers (the coverage table: real vs stubbed) ---------------------------
// SLSNewConnection: REAL — mints a connection id + a fresh per-connection port, records the
// audit pid/version. This is the connection handshake the report's app-lifecycle §2.1 pins.
static void ws_handle_new_connection(const mach_msg_header_t *req, ws_req_newconn *r,
                                     audit_token_t tok) {
    (void)r; // options is accepted and ignored (0 for every real caller)
    ws_reply_newconn rep;
    memset(&rep, 0, sizeof rep);
    rep.hdr.msgh_bits = MACH_MSGH_BITS_COMPLEX |
                        MACH_MSGH_BITS(MACH_MSGH_BITS_REPLY(req->msgh_bits), 0);
    rep.hdr.msgh_remote_port = req->msgh_remote_port;
    rep.hdr.msgh_local_port = MACH_PORT_NULL;
    rep.hdr.msgh_id = req->msgh_id + 100;
    rep.hdr.msgh_reserved = 0;
    rep.hdr.msgh_size = sizeof rep;
    rep.body.msgh_descriptor_count = 1;

    ws_conn_t *c = calloc(1, sizeof *c);
    if (!c) {
        rep.ret_code = WS_ERR_FAILURE;
        rep.port.name = MACH_PORT_NULL;
        mach_msg((mach_msg_header_t *)&rep, MACH_SEND_MSG, sizeof rep, 0, MACH_PORT_NULL,
                 MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        return;
    }
    c->cid = g_next_cid++;
    c->port = ws_alloc_port();
    c->pid = ws_audit_pid(&tok);
    c->version = ws_audit_pidversion(&tok);
    c->next = g_conns;
    g_conns = c;

    rep.port.name = c->port;
    rep.port.pad1 = 0;
    rep.port.pad2 = 0;
    rep.port.disposition = MACH_MSG_TYPE_COPY_SEND;
    rep.port.type = MACH_MSG_PORT_DESCRIPTOR;
    rep.ret_code = WS_ERR_SUCCESS;
    rep.cid = c->cid;
    mach_msg((mach_msg_header_t *)&rep, MACH_SEND_MSG, sizeof rep, 0, MACH_PORT_NULL,
             MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

static void ws_handle_get_event_port(const mach_msg_header_t *req, ws_req_cid *r) {
    ws_conn_t *c = ws_find_conn(r->cid);
    int32_t ret = WS_ERR_SUCCESS;
    mach_port_t port = MACH_PORT_NULL;
    if (!c) {
        ret = WS_ERR_INVALID_CONNECTION;
    } else {
        if (c->event_port == MACH_PORT_NULL) c->event_port = ws_alloc_port();
        port = c->event_port;
    }
    ws_reply_port rep;
    ws_send_port(req, &rep, port, ret);
}

static void ws_dispatch(mach_msg_header_t *req) {
    uint32_t id = req->msgh_id;
    audit_token_t tok = ws_msg_audit_token(req);
    ws_log("[ws] %s (id %u)\n", ws_routine_name(id), id);

    // A REAL SkyLight client sends its routine id as msgh_id (0x74xx-0x7dxx); the
    // hand-rolled validation peer uses the stand-in 1000-1036 table. Route by id.
    if (id >= SLS_REAL_ID_MIN && id < SLS_REAL_ID_MAX) {
        ws_dispatch_real(req, tok);
        return;
    }

    switch (id) {
    // ---- connection bootstrap (6) -----------------------------------------------------
    case WS_R_SLSMainConnectionID: {          // REAL — pinned constant 0x3E8
        ws_reply_cid rep = {{0}, WS_ERR_SUCCESS, WS_MAIN_CONNECTION_ID};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSNewConnection: {             // REAL — mints cid + port
        ws_handle_new_connection(req, (ws_req_newconn *)req, tok);
        break;
    }
    case WS_R_SLSServerPort: {                // REAL — returns the server Mach port
        ws_reply_port rep;
        ws_send_port(req, &rep, g_main_send, WS_ERR_SUCCESS);
        break;
    }
    case WS_R_SLSReleaseConnection: {         // REAL — tears down the per-connection port
        ws_req_cid *r = (ws_req_cid *)req;
        ws_conn_t **pp = &g_conns;
        int32_t ret = WS_ERR_INVALID_CONNECTION;
        while (*pp) {
            if ((*pp)->cid == r->cid) {
                ws_conn_t *c = *pp;
                *pp = c->next;
                ws_free_port(c->port);
                if (c->event_port != MACH_PORT_NULL) ws_free_port(c->event_port);
                if (c->notify_port != MACH_PORT_NULL)
                    mach_port_deallocate(mach_task_self(), c->notify_port);
                free(c);
                ret = WS_ERR_SUCCESS;
                break;
            }
            pp = &(*pp)->next;
        }
        ws_reply_ret rep = {{0}, ret};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSConnectionGetPIDAndVersion: { // REAL — returns recorded pid + version
        ws_req_cid *r = (ws_req_cid *)req;
        ws_reply_pidver rep = {{0}, WS_ERR_INVALID_CONNECTION, 0, 0};
        ws_conn_t *c = ws_find_conn(r->cid);
        if (c) {
            rep.ret_code = WS_ERR_SUCCESS;
            rep.pid = (uint32_t)c->pid;
            rep.version = c->version;
        } else if (r->cid == WS_MAIN_CONNECTION_ID) {
            rep.ret_code = WS_ERR_SUCCESS;
            rep.pid = (uint32_t)ws_audit_pid(&tok);   // the calling process itself
            rep.version = WS_SERVER_VERSION;
        }
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSGetConnectionIDForPSN: {     // STUB — maps every PSN to the main connection
        ws_reply_cid rep = {{0}, WS_ERR_SUCCESS, WS_MAIN_CONNECTION_ID};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    // ---- session + identity (10) ------------------------------------------------------
    case WS_R_SLSSessionCopyCurrentSessionProperties:   // REAL-ish — fixed valid session record
    case WS_R_SLSSessionCopyAllSessionProperties: {
        ws_reply_session rep = {{0}, WS_ERR_SUCCESS,
                                {getuid(), g_session_id, 1, 0}};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSSessionSetCurrentSessionProperties:    // STUB — accept + success
    case WS_R_SLSSessionSetCurrentSessionWorkspacePreferences:
    case WS_R_SLSSessionSetCurrentSessionWorkspaceNotificationConnectionID: {
        ws_reply_ret rep = {{0}, WS_ERR_SUCCESS};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSSetLoginwindowConnection: {  // REAL-ish — records the loginwindow connection
        ws_req_cid *r = (ws_req_cid *)req;
        g_loginwindow_conn = r->cid;
        ws_log("[ws] loginwindow connection = 0x%x\n", g_loginwindow_conn);
        ws_reply_ret rep = {{0}, WS_ERR_SUCCESS};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSSetLoginName: {              // REAL-ish — records the login name
        ws_req_cid_name *r = (ws_req_cid_name *)req;
        strlcpy(g_login_name, r->name, sizeof g_login_name);
        ws_log("[ws] login name = \"%s\"\n", g_login_name);
        ws_reply_ret rep = {{0}, WS_ERR_SUCCESS};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSSessionSwitchToSessionID: {  // STUB — accept + success
        ws_reply_ret rep = {{0}, WS_ERR_SUCCESS};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSSetConnectionProperty: {     // REAL-ish — stores (key,value) per connection
        ws_req_cid_key_val *r = (ws_req_cid_key_val *)req;
        uint32_t vlen = r->vlen <= WS_PROP_VAL_MAX ? r->vlen : WS_PROP_VAL_MAX;
        for (int i = 0; i < WS_MAX_PROPS; i++) {
            if (g_props[i].cid == r->cid && strcmp(g_props[i].key, r->key) == 0) {
                g_props[i].vlen = vlen;
                memcpy(g_props[i].val, r->val, vlen);
                goto set_done;
            }
        }
        for (int i = 0; i < WS_MAX_PROPS; i++) {
            if (g_props[i].cid == 0) {
                g_props[i].cid = r->cid;
                strlcpy(g_props[i].key, r->key, sizeof g_props[i].key);
                g_props[i].vlen = vlen;
                memcpy(g_props[i].val, r->val, vlen);
                break;
            }
        }
    set_done: {
        ws_reply_ret rep = {{0}, WS_ERR_SUCCESS};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    }
    case WS_R_SLSCopyConnectionProperty: {    // REAL-ish — returns the stored value
        ws_req_cid_key *r = (ws_req_cid_key *)req;
        ws_reply_propval rep;
        memset(&rep, 0, sizeof rep);
        rep.ret_code = WS_ERR_NOT_FOUND;
        rep.vlen = 0;
        for (int i = 0; i < WS_MAX_PROPS; i++) {
            if (g_props[i].cid == r->cid && strcmp(g_props[i].key, r->key) == 0) {
                rep.ret_code = WS_ERR_SUCCESS;
                rep.vlen = g_props[i].vlen;
                memcpy(rep.val, g_props[i].val, g_props[i].vlen);
                break;
            }
        }
        mach_msg_size_t sz = (mach_msg_size_t)(offsetof(ws_reply_propval, val) + rep.vlen);
        ws_send_simple(req, &rep.hdr, sz);
        break;
    }
    // ---- notifications (4) ------------------------------------------------------------
    case WS_R_SLSRegisterConnectionNotifyProc: // REAL-ish — records the notify port
    case WS_R_SLSRegisterNotifyProc: {
        ws_req_cid_port *r = (ws_req_cid_port *)req;
        ws_conn_t *c = ws_find_conn(r->cid);
        int32_t ret = WS_ERR_INVALID_CONNECTION;
        if (c) {
            if (c->notify_port != MACH_PORT_NULL)
                mach_port_deallocate(mach_task_self(), c->notify_port);
            c->notify_port = r->port.name;    // the COPY_SEND right the client sent
            ret = WS_ERR_SUCCESS;
        } else {
            mach_port_deallocate(mach_task_self(), r->port.name);
        }
        ws_reply_ret rep = {{0}, ret};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSRemoveConnectionNotifyProc:  // STUB — clears + success
    case WS_R_SLSRemoveNotifyProc: {
        ws_req_cid *r = (ws_req_cid *)req;
        ws_conn_t *c = ws_find_conn(r->cid);
        if (c && c->notify_port != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), c->notify_port);
            c->notify_port = MACH_PORT_NULL;
        }
        ws_reply_ret rep = {{0}, WS_ERR_SUCCESS};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    // ---- events (1) -------------------------------------------------------------------
    case WS_R_SLSGetEventPort: {             // REAL — returns a per-connection event port
        ws_handle_get_event_port(req, (ws_req_cid *)req);
        break;
    }
    // ---- display enumeration (8) ------------------------------------------------------
    case WS_R_SLSMainDisplayID: {            // REAL — pinned main display id
        ws_reply_did rep = {{0}, WS_ERR_SUCCESS, WS_MAIN_DISPLAY_ID};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSGetDisplayList:             // REAL — non-empty [main]
    case WS_R_SLSGetPotentiallyActiveDisplayList: {
        ws_reply_didlist rep;
        memset(&rep, 0, sizeof rep);
        rep.ret_code = WS_ERR_SUCCESS;
        rep.count = WS_DISPLAY_COUNT;
        rep.dids[0] = WS_MAIN_DISPLAY_ID;
        mach_msg_size_t sz = (mach_msg_size_t)(offsetof(ws_reply_didlist, dids) +
                                               rep.count * sizeof(uint32_t));
        ws_send_simple(req, &rep.hdr, sz);
        break;
    }
    case WS_R_SLSGetDisplayBounds: {         // REAL — fixed 1920x1080 @ origin
        ws_reply_rect rep = {{0}, WS_ERR_SUCCESS,
                             {0, 0, WS_DISPLAY_WIDTH, WS_DISPLAY_HEIGHT}};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSCopyDisplayUUID: {          // REAL — fixed uuid, round-trips with the lookup
        ws_reply_uuid rep;
        memset(&rep, 0, sizeof rep);
        rep.ret_code = WS_ERR_SUCCESS;
        static const uint8_t uuid[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                         0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
        memcpy(rep.uuid.b, uuid, 16);
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSGetDisplayForUUID: {        // REAL — maps the fixed uuid back to main display
        ws_reply_did rep = {{0}, WS_ERR_SUCCESS, WS_MAIN_DISPLAY_ID};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSGetCurrentDisplayMode: {    // REAL — fixed mode
        ws_reply_mode rep = {{0}, WS_ERR_SUCCESS,
                             {WS_DISPLAY_WIDTH, WS_DISPLAY_HEIGHT, WS_DISPLAY_REFRESH_HZ,
                              WS_DISPLAY_DEPTH, 0}};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSCopyActiveMenuBarDisplayIdentifier: {  // REAL — main display
        ws_reply_did rep = {{0}, WS_ERR_SUCCESS, WS_MAIN_DISPLAY_ID};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    // ---- window + transaction stub layer (8) -----------------------------------------
    case WS_R_SLSNewWindowWithOpaqueShape:   // REAL-ish — mints a stable window id
    case WS_R_SLSNewWindowWithOpaqueShapeAndContext: {
        ws_req_cid_rect *r = (ws_req_cid_rect *)req;
        ws_window_t *w = calloc(1, sizeof *w);
        if (!w) {
            ws_reply_wid rep = {{0}, WS_ERR_FAILURE, 0};
            ws_send_simple(req, &rep.hdr, sizeof rep);
            break;
        }
        w->wid = g_next_wid++;
        w->cid = r->cid;
        w->rect = r->rect;
        w->next = g_windows;
        g_windows = w;
        ws_reply_wid rep = {{0}, WS_ERR_SUCCESS, w->wid};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSOrderWindow: {              // STUB — accept + success (ordering is a no-op)
        ws_reply_ret rep = {{0}, WS_ERR_SUCCESS};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSSetWindowLevel: {           // REAL-ish — stores the level
        ws_req_cid_wid_level *r = (ws_req_cid_wid_level *)req;
        ws_window_t *w = ws_find_window(r->wid);
        ws_reply_ret rep = {{0}, WS_ERR_INVALID_WINDOW};
        if (w) { w->level = r->level; rep.ret_code = WS_ERR_SUCCESS; }
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSSetWindowTags: {            // REAL-ish — stores the tags
        ws_req_cid_wid_tags *r = (ws_req_cid_wid_tags *)req;
        ws_window_t *w = ws_find_window(r->wid);
        ws_reply_ret rep = {{0}, WS_ERR_INVALID_WINDOW};
        if (w) { w->tags = r->tags; rep.ret_code = WS_ERR_SUCCESS; }
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSReleaseWindow: {            // REAL-ish — frees the window id
        ws_req_cid_wid *r = (ws_req_cid_wid *)req;
        ws_window_t **pp = &g_windows;
        int32_t ret = WS_ERR_INVALID_WINDOW;
        while (*pp) {
            if ((*pp)->wid == r->wid) {
                ws_window_t *w = *pp;
                *pp = w->next;
                free(w);
                ret = WS_ERR_SUCCESS;
                break;
            }
            pp = &(*pp)->next;
        }
        ws_reply_ret rep = {{0}, ret};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSBindSurface: {              // REAL-ish — binds an IOSurface + surfacing handoff
        ws_req_cid_wid_surface *r = (ws_req_cid_wid_surface *)req;
        ws_window_t *w = ws_find_window(r->wid);
        int32_t ret = WS_ERR_INVALID_WINDOW;
        if (w) {
            w->surface_id = r->surface_id;
            w->has_surface = 1;
            ret = WS_ERR_SUCCESS;
            ws_log("[ws] SLSBindSurface window=%u surface=%u type=%u %dx%d\n",
                   r->wid, r->surface_id, r->type, r->w, r->h);
            // The surfacing handoff: (window id + IOSurface id) -> ws_mac_surface_present,
            // which drives the existing mac-app surfacing path (wlsrv.c mac_surface_*) and
            // presents the window as a native Wayland window in Hyprland.
            ws_mac_surface_present(r->wid, r->surface_id);
        }
        ws_reply_ret rep = {{0}, ret};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSTransactionCreate: {        // REAL-ish — mints a transaction ref
        ws_reply_tid rep = {{0}, WS_ERR_SUCCESS, g_next_tid++};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    case WS_R_SLSTransactionCommit: {        // STUB — accept + success (apply is a no-op)
        ws_reply_ret rep = {{0}, WS_ERR_SUCCESS};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    default: {                               // unknown routine -> MIG_BAD_ID-shaped error
        ws_reply_ret rep = {{0}, WS_ERR_ILLEGAL_ARGUMENT};
        ws_send_simple(req, &rep.hdr, sizeof rep);
        break;
    }
    }
}

// ---- server loop ----------------------------------------------------------------------
static void ws_on_signal(int sig) { (void)sig; g_stop = 1; }

// Install the warm-switch signal handlers EARLY (before the check-in), so a SIGTERM/SIGINT
// during startup/check-in also results in a clean exit(0) rather than death-by-signal. The
// clean exit is what the boot chooser keys on to hand the service back to the real WindowServer.
static void ws_init_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = ws_on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int ws_server_run(mach_port_t main_port) {
    ws_init_signals();

    // 8 KB is comfortably larger than any request (largest is the SLSNewConnection handshake
    // 0x80001513 + 0x400 code-identity blob) plus the format-0 audit trailer plus descriptors.
    uint8_t buf[16384];
    mach_msg_options_t rcv = MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_TIMEOUT |
                             MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT) |
                             MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0);

    // Receive on the port set (main + every per-connection port). When the set is in use, a
    // real client's follow-up calls (sent to the per-connection port) land here too.
    mach_port_t rcv_port = (g_port_set != MACH_PORT_NULL) ? g_port_set : main_port;
    ws_log("[ws] server loop on %s 0x%x\n",
           (g_port_set != MACH_PORT_NULL) ? "port set" : "port", rcv_port);
    // A FINITE receive timeout, not MACH_MSG_TIMEOUT_NONE: on this platform a caught signal
    // (SIGTERM/SIGINT) does NOT return MACH_RCV_INTERRUPTED from a blocking mach_msg receive
    // (the receive is silently restarted), so the warm-switch handler would set g_stop and
    // never be observed. The MACH_RCV_TIMEOUT option + WS_RECV_TIMEOUT_MS make mach_msg return
    // MACH_RCV_TIMED_OUT every 100 ms, so the loop re-checks g_stop and the clean-exit path
    // (release + exit 0) is dependable — the boot chooser keys on it to hand the service back
    // to the real WindowServer. (Note: without the MACH_RCV_TIMEOUT option bit, a non-zero
    // timeout value is ignored entirely.)
    while (!g_stop) {
        mach_msg_header_t *req = (mach_msg_header_t *)buf;
        kern_return_t kr = mach_msg(req, rcv, 0, sizeof buf, rcv_port,
                                    WS_RECV_TIMEOUT_MS, MACH_PORT_NULL);
        if (kr == MACH_RCV_TIMED_OUT) continue;   // idle tick: re-check g_stop
        if (kr == MACH_RCV_INTERRUPTED) {
            if (g_stop) break;
            continue;
        }
        if (kr != KERN_SUCCESS) {
            ws_log("[ws] receive error 0x%x\n", kr);
            continue;
        }
        if (req->msgh_local_port == g_main_port) {
            ws_dispatch(req);      // main service port: handshake + stand-in table
        } else {
            ws_dispatch_real(req, ws_msg_audit_token(req));  // per-connection port
        }
    }
    ws_log("[ws] shutting down\n");
    return 0;
}

// ---- reusable serve entry (injected-payload host) --------------------------------------
// The same setup main() performs after the bootstrap check-in, factored out so the
// WindowServer-injected payload can host the stand-in's ~37 functions + real SLS protocol
// without re-running main()'s check-in/register. See windowserver.h.
int ws_server_serve(mach_port_t main_port) {
    if (main_port == MACH_PORT_NULL) return 1;

    // The test/check-in path already holds a send right; the injected host gets a bare
    // receive right from the injector, so insert one before handing it out via SLSServerPort.
    mach_port_insert_right(mach_task_self(), main_port, main_port, MACH_MSG_TYPE_MAKE_SEND);
    g_main_port = main_port;
    g_main_send = main_port;

    // One receive loop serves the main port AND every per-connection port the handshake
    // mints: allocate a port set, seed it with the main port, and add connection ports as
    // they appear (ws_dispatch_real moves them in when g_port_set != MACH_PORT_NULL).
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &g_port_set) == KERN_SUCCESS)
        mach_port_move_member(mach_task_self(), g_main_port, g_port_set);
    else
        g_port_set = MACH_PORT_NULL;

    ws_log("[ws] serving on main port 0x%x via %s\n",
           g_main_port, (g_port_set != MACH_PORT_NULL) ? "port set" : "port");
    return ws_server_run(g_main_port);
}

// ---- entry point ----------------------------------------------------------------------
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    g_verbose = getenv("WS_VERBOSE") == NULL || atoi(getenv("WS_VERBOSE")) != 0;
    ws_init_signals();   // warm-switch: SIGTERM/SIGINT -> clean exit(0) even during startup

    mach_port_t bp = MACH_PORT_NULL;
    if (task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &bp) != KERN_SUCCESS) {
        fprintf(stderr, "[ws] FATAL: no bootstrap port\n");
        return 1;
    }

    const char *service_name = getenv(SHIMMY_SERVICE_MAIN_ENV);
    const char *active_name  = getenv(SHIMMY_SERVICE_ACTIVE_ENV);
    if (!service_name) service_name = getenv("WS_SERVICE_NAME");
    if (!active_name)  active_name  = getenv("WS_ACTIVE_NAME");
    if (!service_name) service_name = WS_TEST_SERVICE_NAME;
    if (!active_name)  active_name  = WS_TEST_ACTIVE_NAME;

    const char *preheld_env = getenv(SHIMMY_SERVICE_PREHELD_ENV);
    int preheld = (preheld_env && preheld_env[0] == '1' && preheld_env[1] == '\0');

    kern_return_t kr;
    if (preheld) {
        // The chooser already claimed the services at boot; receive them over the `.active`
        // rendezvous. No second check-in (that would re-open the race); no root gate needed
        // because we only adopt rights the (root) chooser already holds.
        fprintf(stderr, "[ws] pre-held: receiving already-claimed '%s' (+ '%s') from the chooser\n",
                service_name, active_name);
        kr = ws_bootstrap_receive_preheld(bp, active_name, &g_main_port, &g_active_port);
    } else if (getenv("RDNATIVE_WINDOWSERVER_SYSTEM")) {
        // The REAL takeover path. Only as root, only against launchd-owned names; NOT used in
        // validation. Mirrors the report: check_in main, then .active.
        if (geteuid() != 0) {
            fprintf(stderr, "[ws] REFUSING: RDNATIVE_WINDOWSERVER_SYSTEM requires root (have uid %d)\n",
                    (int)geteuid());
            return 1;
        }
        fprintf(stderr, "[ws] WARNING: checking in the REAL WindowServer services "
                        "(com.apple.windowserver + .active)\n");
        kr = ws_bootstrap_check_in_system(bp, &g_main_port, &g_active_port);
        service_name = WS_REAL_SERVICE_NAME;
        active_name = WS_REAL_ACTIVE_NAME;
    } else {
        kr = ws_bootstrap_register_test(bp, service_name, active_name, &g_main_port, &g_active_port);
    }

    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[ws] FATAL: %s '%s' failed: 0x%x (%s)\n",
                preheld ? "handoff" : "check-in", service_name, kr, bootstrap_strerror(kr));
        return 1;
    }

    // SLSServerPort hands out a send right to the main port. The test path already holds one
    // (it inserts a send right so bootstrap_register can COPY_SEND it); the system and pre-held
    // paths start with a bare receive right, so give ourselves a send right there.
    if (preheld || getenv("RDNATIVE_WINDOWSERVER_SYSTEM")) {
        mach_port_insert_right(mach_task_self(), g_main_port, g_main_port, MACH_MSG_TYPE_MAKE_SEND);
    }
    g_main_send = g_main_port;

    // One receive loop serves the main port AND every per-connection port the handshake mints:
    // allocate a port set, seed it with the main port, and add connection ports as they appear.
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &g_port_set) == KERN_SUCCESS)
        mach_port_move_member(mach_task_self(), g_main_port, g_port_set);
    else
        g_port_set = MACH_PORT_NULL;

    fprintf(stderr, preheld
            ? "[ws] pre-held '%s' (port 0x%x) + '%s' (port 0x%x) from the chooser\n"
            : "[ws] checked in '%s' (port 0x%x), then '%s' (port 0x%x)\n",
            service_name, g_main_port, active_name, g_active_port);

    int rc = ws_server_run(g_main_port);

    // Release the two receive rights the check-in returned. In system mode this is the
    // warm-switch "release": once this process exits, com.apple.windowserver (+ .active) goes
    // dead and the real WindowServer can re-check-in and take over.
    ws_free_port(g_main_port);
    if (g_active_port != MACH_PORT_NULL) ws_free_port(g_active_port);
    if (rc == 0)
        fprintf(stderr, "[ws] warm-switch: released '%s', exiting cleanly\n", service_name);
    return rc;
}
