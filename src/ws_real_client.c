// ws_real_client.c — a RAW MIG client that speaks the REAL SkyLight SLS wire protocol
// (the 0x80001513/0x1513 envelope + the real SLS routine msgh_ids), pointed at the shim's
// *test* bootstrap name. This is the closest thing to a real CoreGraphics/AppKit client
// that can run headlessly without taking over the live WindowServer: it uses the exact
// SLSNewConnection handshake (magic 0x80001513, msgh_id 0x7d00) and the real reply ids
// (request + 0x64), then drives SLSConnectionGetPIDAndVersion / SLSNewWindow /
// SLSOrderWindow / SLSGetWindowList over the returned per-connection port.
//
// It never touches `com.apple.windowserver`.
#include "windowserver.h"

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (cond) { printf("  PASS  %s\n", msg); g_pass++; }    \
        else      { printf("  FAIL  %s\n", msg); g_fail++; }    \
    } while (0)

// ---- real-protocol wire structs (mirror the decoded layout; see windowserver.h) -------
// Handshake request: COMPLEX (0x80001513) + a 0x400-byte code-identity/audit blob.
typedef struct {
    mach_msg_header_t hdr;
    mach_msg_body_t body;                 // descriptor count = 0
    uint8_t code_identity[0x400];         // the audit payload (zeros here)
} real_req_newconn;

// Handshake reply: header + body(2) + two MOVE_SEND ports + NDR + cid.
typedef struct {
    mach_msg_header_t hdr;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port0;
    mach_msg_port_descriptor_t port1;
    uint64_t ndr;
    uint32_t cid;
} real_reply_newconn;

typedef struct { mach_msg_header_t hdr; uint64_t tok; uint32_t cid; } real_req_cid;
typedef struct { mach_msg_header_t hdr; uint64_t tok; uint32_t cid; uint32_t max; } real_req_cid_max;
typedef struct { mach_msg_header_t hdr; uint64_t ndr; int32_t ret_code; uint32_t pid; uint32_t version; } real_reply_pidver;
typedef struct { mach_msg_header_t hdr; uint64_t ndr; int32_t ret_code; uint32_t count; uint32_t wids[WS_MAX_WINDOWS]; } real_reply_widlist;
typedef struct { mach_msg_header_t hdr; uint64_t ndr; int32_t ret_code; uint32_t wid; } real_reply_wid;

// ---- RPC transport (same send-once reply-port mechanics the real client uses) ---------
static kern_return_t real_rpc(mach_port_t server, mach_msg_header_t *req,
                              mach_msg_size_t req_size,
                              mach_msg_header_t *rep, mach_msg_size_t rep_size) {
    mach_port_t reply_recv = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply_recv) != KERN_SUCCESS)
        return KERN_RESOURCE_SHORTAGE;

    req->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    req->msgh_remote_port = server;
    req->msgh_local_port = reply_recv;
    req->msgh_size = req_size;
    req->msgh_reserved = 0;

    kern_return_t kr = mach_msg(req, MACH_SEND_MSG, req_size, 0, MACH_PORT_NULL,
                                MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), reply_recv);
        return kr;
    }
    uint8_t buf[8192];
    kr = mach_msg((mach_msg_header_t *)buf, MACH_RCV_MSG | MACH_RCV_LARGE, 0, sizeof buf,
                  reply_recv, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    mach_port_deallocate(mach_task_self(), reply_recv);
    if (kr != KERN_SUCCESS) return kr;
    mach_msg_size_t got = ((mach_msg_header_t *)buf)->msgh_size;
    if (got > rep_size) got = rep_size;
    memcpy(rep, buf, got);
    return KERN_SUCCESS;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *service_name = getenv("WS_SERVICE_NAME");
    const char *active_name = getenv("WS_ACTIVE_NAME");
    if (!service_name) service_name = WS_TEST_SERVICE_NAME;
    if (!active_name) active_name = WS_TEST_ACTIVE_NAME;

    mach_port_t bp = MACH_PORT_NULL;
    task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &bp);

    printf("== REAL SkyLight SLS wire-protocol client (raw MIG) ==\n");

    // ---- 1. bootstrap_look_up: .active first, then the main port (real client order) --
    name_t n;
    mach_port_t active = MACH_PORT_NULL, server = MACH_PORT_NULL;
    strlcpy(n, active_name, sizeof n);
    kern_return_t kr = bootstrap_look_up(bp, n, &active);
    CHECK(kr == KERN_SUCCESS, "bootstrap_look_up(active) resolves");
    strlcpy(n, service_name, sizeof n);
    kr = bootstrap_look_up(bp, n, &server);
    CHECK(kr == KERN_SUCCESS && server != MACH_PORT_NULL, "bootstrap_look_up(main) resolves");
    printf("  server port = 0x%x\n", server);

    // ---- 2. SLSNewConnection handshake (0x80001513, msgh_id 0x7d00) -------------------
    real_req_newconn nc;
    memset(&nc, 0, sizeof nc);
    nc.hdr.msgh_bits = MACH_MSGH_BITS_COMPLEX |
                       MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    nc.hdr.msgh_id = SLS_R_NewConnection;
    nc.hdr.msgh_remote_port = server;
    nc.hdr.msgh_local_port = MACH_PORT_NULL;   // filled by real_rpc's send-once path? no —
    nc.body.msgh_descriptor_count = 0;
    nc.hdr.msgh_size = sizeof nc;
    nc.hdr.msgh_reserved = 0;

    // The handshake is a *complex* message; send it directly (not via real_rpc, which would
    // overwrite the bits), with the send-once reply port.
    mach_port_t reply_recv = MACH_PORT_NULL;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply_recv);
    nc.hdr.msgh_local_port = reply_recv;
    kr = mach_msg(&nc.hdr, MACH_SEND_MSG, sizeof nc, 0, MACH_PORT_NULL,
                  MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    CHECK(kr == KERN_SUCCESS, "SLSNewConnection handshake sends (0x80001513, id 0x7d00)");

    real_reply_newconn ncrep;
    memset(&ncrep, 0, sizeof ncrep);
    uint8_t rbuf[8192];
    kr = mach_msg((mach_msg_header_t *)rbuf, MACH_RCV_MSG | MACH_RCV_LARGE, 0, sizeof rbuf,
                  reply_recv, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    mach_port_deallocate(mach_task_self(), reply_recv);
    memcpy(&ncrep, rbuf, sizeof ncrep);
    CHECK(kr == KERN_SUCCESS && ncrep.hdr.msgh_id == SLS_R_NewConnection + SLS_REPLY_OFFSET,
          "SLSNewConnection reply id == 0x7d64 (request + 0x64)");
    CHECK(ncrep.cid != 0 && ncrep.cid != WS_MAIN_CONNECTION_ID,
          "SLSNewConnection returns a fresh connection id");
    CHECK(ncrep.port0.name != MACH_PORT_NULL, "SLSNewConnection returns a connection port");
    printf("  connection id = 0x%x, connection port = 0x%x\n", ncrep.cid, ncrep.port0.name);
    uint32_t cid = ncrep.cid;
    mach_port_t conn_port = ncrep.port0.name;

    // ---- 3. SLSConnectionGetPIDAndVersion (id 0x7d01) on the connection port -----------
    real_req_cid pv;
    memset(&pv, 0, sizeof pv);
    pv.hdr.msgh_id = SLS_R_ConnectionGetPIDAndVersion;
    pv.tok = SLS_NDR;
    pv.cid = cid;
    real_reply_pidver pvrep;
    memset(&pvrep, 0, sizeof pvrep);
    kr = real_rpc(conn_port, &pv.hdr, sizeof pv, &pvrep.hdr, sizeof pvrep);
    CHECK(kr == KERN_SUCCESS && pvrep.hdr.msgh_id == SLS_R_ConnectionGetPIDAndVersion + SLS_REPLY_OFFSET,
          "GetPIDAndVersion reply id == 0x7d65");
    CHECK(pvrep.ret_code == WS_ERR_SUCCESS && (pid_t)pvrep.pid == getpid(),
          "GetPIDAndVersion returns the caller pid over the real envelope");
    printf("  pid = %u, version = 0x%x\n", pvrep.pid, pvrep.version);

    // ---- 4. SLSNewWindowWithOpaqueShape -> CreateWindow (id 0x7637) ------------------
    sls_req_createwindow nw;
    memset(&nw, 0, sizeof nw);
    nw.hdr.msgh_id = SLS_R_NewWindowWithOpaqueShape;
    nw.tok = SLS_NDR;
    nw.op = 0xd;               // the classic SLSNewWindow operation code
    real_reply_wid nwrep;
    memset(&nwrep, 0, sizeof nwrep);
    kr = real_rpc(conn_port, &nw.hdr, sizeof nw, &nwrep.hdr, sizeof nwrep);
    CHECK(kr == KERN_SUCCESS && nwrep.hdr.msgh_id == SLS_R_NewWindowWithOpaqueShape + SLS_REPLY_OFFSET,
          "CreateWindow reply id == 0x769b (request + 0x64)");
    CHECK(nwrep.ret_code == WS_ERR_SUCCESS && nwrep.wid >= WS_FIRST_WINDOW_ID,
          "CreateWindow (0x7637) mints a window id");
    uint32_t wid = nwrep.wid;
    printf("  window id = %u\n", wid);

    // ---- 4b. SLSBindSurface (id 0x758d) ----------------------------------------------
    // The real-envelope bind: attach a synthetic IOSurface id to the window. The stand-in
    // records the binding and hands (window id, surface id) to ws_mac_surface_present — the
    // surfacing handoff that the server logs as "would create wl_surface + xdg_toplevel".
    // The real wire args are (windowID, surfaceID, type, w, h) — NO connection id (the
    // connection is identified by the port the message is sent to).
    sls_req_bindsurface bs;
    memset(&bs, 0, sizeof bs);
    bs.hdr.msgh_id = SLS_R_BindSurface;
    bs.tok = SLS_NDR;
    bs.wid = wid;
    bs.surface_id = 0xC0FFEEu;   // synthetic known IOSurface id
    bs.type = 1;                 // kCGSWindowSurfaceType_host (opaque to the stand-in)
    bs.w = 400; bs.h = 300;
    sls_reply_ret bsrep;
    memset(&bsrep, 0, sizeof bsrep);
    kr = real_rpc(conn_port, &bs.hdr, sizeof bs, &bsrep.hdr, sizeof bsrep);
    CHECK(kr == KERN_SUCCESS && bsrep.hdr.msgh_id == SLS_R_BindSurface + SLS_REPLY_OFFSET,
          "BindSurface reply id == 0x75f1 (request + 0x64)");
    CHECK(bsrep.ret_code == WS_ERR_SUCCESS,
          "BindSurface (real envelope) binds a surface id and triggers the surfacing handoff");
    printf("  bound surface_id = 0x%x on window %u (real envelope)\n", bs.surface_id, wid);

    // ---- 5. SLSOrderWindow (id 0x753a — the single-window "OrderWindow" routine) ------
    sls_req_orderwindow ow;
    memset(&ow, 0, sizeof ow);
    ow.hdr.msgh_id = SLS_R_OrderWindow;
    ow.tok = SLS_NDR;
    ow.wid = wid;
    ow.place = 1;              // kCGSOrderAbove
    ow.other = 0;
    sls_reply_ret owrep;
    memset(&owrep, 0, sizeof owrep);
    kr = real_rpc(conn_port, &ow.hdr, sizeof ow, &owrep.hdr, sizeof owrep);
    CHECK(kr == KERN_SUCCESS && owrep.hdr.msgh_id == SLS_R_OrderWindow + SLS_REPLY_OFFSET,
          "OrderWindow reply id == 0x759e (request + 0x64)");
    CHECK(owrep.ret_code == WS_ERR_SUCCESS, "OrderWindow (0x753a) succeeds");

    // ---- 6. SLSGetWindowList (id 0x7573) — the "list" round-trip ----------------------
    real_req_cid_max wl;
    memset(&wl, 0, sizeof wl);
    wl.hdr.msgh_id = SLS_R_GetWindowList;
    wl.tok = SLS_NDR;
    wl.cid = cid;
    wl.max = WS_MAX_WINDOWS;
    real_reply_widlist wlrep;
    memset(&wlrep, 0, sizeof wlrep);
    kr = real_rpc(conn_port, &wl.hdr, sizeof wl, &wlrep.hdr, sizeof wlrep);
    int found = 0;
    for (uint32_t i = 0; i < wlrep.count && i < WS_MAX_WINDOWS; i++)
        if (wlrep.wids[i] == wid) found = 1;
    CHECK(kr == KERN_SUCCESS && wlrep.ret_code == WS_ERR_SUCCESS && found,
          "GetWindowList returns the window we just created");
    printf("  window list: count = %u\n", wlrep.count);

    // ---- 6b. SLSTransactionCommit -> TransactionCommitSync (id 0x76de) ----------------
    sls_req_transaction tc;
    memset(&tc, 0, sizeof tc);
    tc.hdr.msgh_id = SLS_R_TransactionCommit;
    tc.tok = SLS_NDR;
    tc.tid = 1;
    sls_reply_ret tcrep;
    memset(&tcrep, 0, sizeof tcrep);
    kr = real_rpc(conn_port, &tc.hdr, sizeof tc, &tcrep.hdr, sizeof tcrep);
    CHECK(kr == KERN_SUCCESS && tcrep.hdr.msgh_id == SLS_R_TransactionCommit + SLS_REPLY_OFFSET,
          "TransactionCommit reply id == 0x7742 (request + 0x64)");
    CHECK(tcrep.ret_code == WS_ERR_SUCCESS, "TransactionCommitSync (0x76de) succeeds");

    // ---- 6c. SLSReleaseWindow -> TerminateWindow (id 0x7535) ---------------------------
    sls_req_terminatewindow tw;
    memset(&tw, 0, sizeof tw);
    tw.hdr.msgh_id = SLS_R_ReleaseWindow;
    tw.tok = SLS_NDR;
    tw.wid = wid;
    sls_reply_ret twrep;
    memset(&twrep, 0, sizeof twrep);
    kr = real_rpc(conn_port, &tw.hdr, sizeof tw, &twrep.hdr, sizeof twrep);
    CHECK(kr == KERN_SUCCESS && twrep.hdr.msgh_id == SLS_R_ReleaseWindow + SLS_REPLY_OFFSET,
          "TerminateWindow reply id == 0x7599 (request + 0x64)");
    CHECK(twrep.ret_code == WS_ERR_SUCCESS, "TerminateWindow (0x7535) releases the window");

    // ---- 6d. the released window is gone from the list ---------------------------------
    memset(&wl, 0, sizeof wl);
    wl.hdr.msgh_id = SLS_R_GetWindowList;
    wl.tok = SLS_NDR;
    wl.cid = cid;
    wl.max = WS_MAX_WINDOWS;
    memset(&wlrep, 0, sizeof wlrep);
    kr = real_rpc(conn_port, &wl.hdr, sizeof wl, &wlrep.hdr, sizeof wlrep);
    found = 0;
    for (uint32_t i = 0; i < wlrep.count && i < WS_MAX_WINDOWS; i++)
        if (wlrep.wids[i] == wid) found = 1;
    CHECK(kr == KERN_SUCCESS && wlrep.ret_code == WS_ERR_SUCCESS && !found,
          "GetWindowList no longer returns the released window");

    // ---- 7. unknown real id -> benign success -----------------------------------------
    real_req_cid unk;
    memset(&unk, 0, sizeof unk);
    unk.hdr.msgh_id = 0x7777;   // a plausible-but-unimplemented SLS id
    unk.tok = SLS_NDR;
    unk.cid = cid;
    real_reply_wid unkrep;
    memset(&unkrep, 0, sizeof unkrep);
    kr = real_rpc(conn_port, &unk.hdr, sizeof unk, &unkrep.hdr, sizeof unkrep);
    CHECK(kr == KERN_SUCCESS && unkrep.ret_code == WS_ERR_SUCCESS,
          "unknown SLS id is answered (no hang)");

    if (conn_port != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), conn_port);

    printf("\n== summary: %d pass, %d fail — REAL client %s ==\n",
           g_pass, g_fail, (g_fail == 0) ? "CONNECTED" : "FAILED");
    return g_fail ? 1 : 0;
}
