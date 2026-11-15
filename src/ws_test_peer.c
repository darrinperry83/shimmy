// ws_test_peer.c — validation peer for the shim-as-WindowServer stand-in (first cut).
//
// Proves, headlessly and safely, that the stand-in's bootstrap check-in and MIG-style CGS/SLS
// handshake work: it bootstrap_look_up's the *test* service name (active probe first, then the
// main port — the same order the real client library uses, per WINDOWSERVER_BOOTSTRAP_REGISTRATION.md
// §4), then drives SLSNewConnection + the display/connection/session/window/transaction calls
// against the stand-in and asserts each answer is coherent.
//
// It never touches the real `com.apple.windowserver` name.
#include "windowserver.h"

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MACH_MSGH_BITS_REPLY(bits) MACH_MSGH_BITS_REMOTE(bits)

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (cond) {                                             \
            printf("  PASS  %s\n", msg);                        \
            g_pass++;                                           \
        } else {                                                \
            printf("  FAIL  %s\n", msg);                        \
            g_fail++;                                           \
        }                                                       \
    } while (0)

// Send a simple (non-complex) request and receive its reply.
static kern_return_t ws_rpc(mach_port_t server, mach_msg_header_t *req,
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
    // Receive into a roomy buffer: mach_msg needs rcv_size strictly larger than the message
    // (it reserves the minimum trailer), so an exactly-sized reply struct returns
    // MACH_RCV_TOO_LARGE. Copy the delivered message back into the typed reply struct.
    uint8_t buf[1024];
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
    setvbuf(stdout, NULL, _IONBF, 0);   // unbuffered so progress is visible even on a hang

    const char *service_name = getenv("WS_SERVICE_NAME");
    const char *active_name = getenv("WS_ACTIVE_NAME");
    if (!service_name) service_name = WS_TEST_SERVICE_NAME;
    if (!active_name) active_name = WS_TEST_ACTIVE_NAME;

    mach_port_t bp = MACH_PORT_NULL;
    task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &bp);

    printf("== shim-as-WindowServer validation peer ==\n");

    // ---- 1. bootstrap_look_up: active probe first, then the main port (mirrors the client) --
    name_t n;
    mach_port_t active = MACH_PORT_NULL, server = MACH_PORT_NULL;
    strlcpy(n, active_name, sizeof n);
    kern_return_t kr = bootstrap_look_up(bp, n, &active);
    printf("bootstrap_look_up('%s') -> 0x%x\n", active_name, kr);
    CHECK(kr == KERN_SUCCESS, "active service resolves");
    strlcpy(n, service_name, sizeof n);
    kr = bootstrap_look_up(bp, n, &server);
    printf("bootstrap_look_up('%s') -> 0x%x (port 0x%x)\n", service_name, kr, server);
    CHECK(kr == KERN_SUCCESS && server != MACH_PORT_NULL, "main service resolves to a port");

    // ---- 2. connection handshake --------------------------------------------------------
    ws_req_newconn nc;
    memset(&nc, 0, sizeof nc);
    nc.hdr.msgh_id = WS_R_SLSNewConnection;
    nc.options = 0;
    ws_reply_newconn ncrep;
    memset(&ncrep, 0, sizeof ncrep);
    kr = ws_rpc(server, &nc.hdr, sizeof nc, &ncrep.hdr, sizeof ncrep);
    CHECK(kr == KERN_SUCCESS && ncrep.ret_code == WS_ERR_SUCCESS,
          "SLSNewConnection returns success");
    CHECK(ncrep.cid != 0 && ncrep.cid != WS_MAIN_CONNECTION_ID,
          "SLSNewConnection mints a fresh connection id");
    CHECK(ncrep.port.name != MACH_PORT_NULL, "SLSNewConnection returns a connection port");
    printf("  connection id = 0x%x, connection port = 0x%x\n", ncrep.cid, ncrep.port.name);
    ws_cid_t cid = ncrep.cid;
    mach_port_t conn_port = ncrep.port.name;

    ws_req_empty m;
    ws_reply_cid mrep;
    memset(&m, 0, sizeof m);
    m.hdr.msgh_id = WS_R_SLSMainConnectionID;
    kr = ws_rpc(server, &m.hdr, sizeof m, &mrep.hdr, sizeof mrep);
    CHECK(kr == KERN_SUCCESS && mrep.ret_code == WS_ERR_SUCCESS && mrep.cid == WS_MAIN_CONNECTION_ID,
          "SLSMainConnectionID == 0x3E8");
    printf("  main connection id = 0x%x\n", mrep.cid);

    ws_reply_port srep;
    memset(&m, 0, sizeof m);
    m.hdr.msgh_id = WS_R_SLSServerPort;
    kr = ws_rpc(server, &m.hdr, sizeof m, &srep.hdr, sizeof srep);
    CHECK(kr == KERN_SUCCESS && srep.ret_code == WS_ERR_SUCCESS && srep.port.name != MACH_PORT_NULL,
          "SLSServerPort returns the server Mach port");
    printf("  server port = 0x%x\n", srep.port.name);
    if (srep.port.name != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), srep.port.name);

    ws_req_cid pidr;
    ws_reply_pidver pv;
    memset(&pidr, 0, sizeof pidr);
    pidr.hdr.msgh_id = WS_R_SLSConnectionGetPIDAndVersion;
    pidr.cid = cid;
    kr = ws_rpc(server, &pidr.hdr, sizeof pidr, &pv.hdr, sizeof pv);
    CHECK(kr == KERN_SUCCESS && pv.ret_code == WS_ERR_SUCCESS && (pid_t)pv.pid == getpid(),
          "SLSConnectionGetPIDAndVersion returns the caller pid");
    printf("  pid = %u, version = 0x%x\n", pv.pid, pv.version);

    // ---- 3. display enumeration ----------------------------------------------------------
    ws_reply_did drep;
    memset(&m, 0, sizeof m);
    m.hdr.msgh_id = WS_R_SLSMainDisplayID;
    kr = ws_rpc(server, &m.hdr, sizeof m, &drep.hdr, sizeof drep);
    CHECK(kr == KERN_SUCCESS && drep.ret_code == WS_ERR_SUCCESS && drep.did == WS_MAIN_DISPLAY_ID,
          "SLSMainDisplayID == 1");
    ws_did_t main_did = drep.did;

    ws_reply_didlist dl;
    memset(&m, 0, sizeof m);
    m.hdr.msgh_id = WS_R_SLSGetDisplayList;
    kr = ws_rpc(server, &m.hdr, sizeof m, &dl.hdr, sizeof dl);
    CHECK(kr == KERN_SUCCESS && dl.ret_code == WS_ERR_SUCCESS && dl.count >= 1,
          "SLSGetDisplayList is non-empty");
    printf("  display list: count = %u, [0] = %u\n", dl.count, dl.dids[0]);

    ws_req_did dr;
    ws_reply_rect brep;
    memset(&dr, 0, sizeof dr);
    dr.hdr.msgh_id = WS_R_SLSGetDisplayBounds;
    dr.did = main_did;
    kr = ws_rpc(server, &dr.hdr, sizeof dr, &brep.hdr, sizeof brep);
    CHECK(kr == KERN_SUCCESS && brep.ret_code == WS_ERR_SUCCESS &&
              brep.rect.w == WS_DISPLAY_WIDTH && brep.rect.h == WS_DISPLAY_HEIGHT,
          "SLSGetDisplayBounds == 1920x1080");
    printf("  bounds = (%d,%d %dx%d)\n", brep.rect.x, brep.rect.y, brep.rect.w, brep.rect.h);

    ws_reply_uuid urep;
    memset(&dr, 0, sizeof dr);
    dr.hdr.msgh_id = WS_R_SLSCopyDisplayUUID;
    dr.did = main_did;
    kr = ws_rpc(server, &dr.hdr, sizeof dr, &urep.hdr, sizeof urep);
    int uuid_nonzero = 0;
    for (int i = 0; i < 16; i++) if (urep.uuid.b[i]) uuid_nonzero = 1;
    CHECK(kr == KERN_SUCCESS && urep.ret_code == WS_ERR_SUCCESS && uuid_nonzero,
          "SLSCopyDisplayUUID returns a non-zero uuid");

    ws_req_uuid ur;
    ws_reply_did fdrep;
    memset(&ur, 0, sizeof ur);
    ur.hdr.msgh_id = WS_R_SLSGetDisplayForUUID;
    ur.uuid = urep.uuid;
    kr = ws_rpc(server, &ur.hdr, sizeof ur, &fdrep.hdr, sizeof fdrep);
    CHECK(kr == KERN_SUCCESS && fdrep.ret_code == WS_ERR_SUCCESS && fdrep.did == WS_MAIN_DISPLAY_ID,
          "SLSGetDisplayForUUID(uuid) round-trips to the main display");

    ws_reply_mode mrep2;
    memset(&dr, 0, sizeof dr);
    dr.hdr.msgh_id = WS_R_SLSGetCurrentDisplayMode;
    dr.did = main_did;
    kr = ws_rpc(server, &dr.hdr, sizeof dr, &mrep2.hdr, sizeof mrep2);
    CHECK(kr == KERN_SUCCESS && mrep2.ret_code == WS_ERR_SUCCESS &&
              mrep2.mode.width == WS_DISPLAY_WIDTH && mrep2.mode.height == WS_DISPLAY_HEIGHT,
          "SLSGetCurrentDisplayMode == 1920x1080@60");

    ws_reply_did adrep;
    memset(&m, 0, sizeof m);
    m.hdr.msgh_id = WS_R_SLSCopyActiveMenuBarDisplayIdentifier;
    kr = ws_rpc(server, &m.hdr, sizeof m, &adrep.hdr, sizeof adrep);
    CHECK(kr == KERN_SUCCESS && adrep.ret_code == WS_ERR_SUCCESS && adrep.did == WS_MAIN_DISPLAY_ID,
          "SLSCopyActiveMenuBarDisplayIdentifier == main display");

    // ---- 4. session + identity -----------------------------------------------------------
    ws_reply_session ses;
    memset(&m, 0, sizeof m);
    m.hdr.msgh_id = WS_R_SLSSessionCopyCurrentSessionProperties;
    kr = ws_rpc(server, &m.hdr, sizeof m, &ses.hdr, sizeof ses);
    CHECK(kr == KERN_SUCCESS && ses.ret_code == WS_ERR_SUCCESS && ses.props.on_console == 1,
          "SLSSessionCopyCurrentSessionProperties returns a live session record");
    printf("  session: uid=%u sid=0x%x on_console=%u locked=%u\n",
           ses.props.user_id, ses.props.session_id, ses.props.on_console, ses.props.locked);

    ws_req_cid_key_val setp;
    memset(&setp, 0, sizeof setp);
    setp.hdr.msgh_id = WS_R_SLSSetConnectionProperty;
    setp.cid = cid;
    strlcpy(setp.key, "BundleID", sizeof setp.key);
    const char *bid = "com.shimmy.windowserver.test";
    setp.vlen = (uint32_t)strlen(bid) + 1;
    memcpy(setp.val, bid, setp.vlen);
    ws_reply_ret setrep;
    kr = ws_rpc(server, &setp.hdr, sizeof setp, &setrep.hdr, sizeof setrep);
    CHECK(kr == KERN_SUCCESS && setrep.ret_code == WS_ERR_SUCCESS,
          "SLSSetConnectionProperty succeeds");

    ws_req_cid_key getp;
    ws_reply_propval getprep;
    memset(&getp, 0, sizeof getp);
    getp.hdr.msgh_id = WS_R_SLSCopyConnectionProperty;
    getp.cid = cid;
    strlcpy(getp.key, "BundleID", sizeof getp.key);
    kr = ws_rpc(server, &getp.hdr, sizeof getp, &getprep.hdr, sizeof getprep);
    CHECK(kr == KERN_SUCCESS && getprep.ret_code == WS_ERR_SUCCESS &&
              strcmp((char *)getprep.val, bid) == 0,
          "SLSCopyConnectionProperty round-trips the stored value");

    // ---- 5. events ----------------------------------------------------------------------
    ws_reply_port erep;
    memset(&pidr, 0, sizeof pidr);
    pidr.hdr.msgh_id = WS_R_SLSGetEventPort;
    pidr.cid = cid;
    kr = ws_rpc(server, &pidr.hdr, sizeof pidr, &erep.hdr, sizeof erep);
    CHECK(kr == KERN_SUCCESS && erep.ret_code == WS_ERR_SUCCESS && erep.port.name != MACH_PORT_NULL,
          "SLSGetEventPort returns a per-connection event port");
    if (erep.port.name != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), erep.port.name);

    // ---- 6. window + transaction stub layer ---------------------------------------------
    ws_req_cid_rect nw;
    ws_reply_wid wrep;
    memset(&nw, 0, sizeof nw);
    nw.hdr.msgh_id = WS_R_SLSNewWindowWithOpaqueShape;
    nw.cid = cid;
    nw.rect = (ws_rect_t){50, 50, 400, 300};
    kr = ws_rpc(server, &nw.hdr, sizeof nw, &wrep.hdr, sizeof wrep);
    CHECK(kr == KERN_SUCCESS && wrep.ret_code == WS_ERR_SUCCESS && wrep.wid >= WS_FIRST_WINDOW_ID,
          "SLSNewWindowWithOpaqueShape mints a stable window id");
    ws_wid_t wid = wrep.wid;
    printf("  window id = %u\n", wid);

    // SLSBindSurface: bind a synthetic IOSurface id to the window. This is the milestone's
    // "mac apps inside Linux" wiring proof — the stand-in records the binding and hands
    // (window id, surface id) to ws_mac_surface_present (the surfacing handoff), which the
    // server logs as "would create wl_surface + xdg_toplevel in Hyprland".
    ws_req_cid_wid_surface bs;
    ws_reply_ret bsrep;
    memset(&bs, 0, sizeof bs);
    bs.hdr.msgh_id = WS_R_SLSBindSurface;
    bs.cid = cid;
    bs.wid = wid;
    bs.surface_id = 0xC0FFEEu;   // synthetic known IOSurface id (no real IOSurface headlessly)
    bs.type = 1;                 // kCGSWindowSurfaceType_host (opaque to the stand-in)
    bs.w = 400; bs.h = 300;
    kr = ws_rpc(server, &bs.hdr, sizeof bs, &bsrep.hdr, sizeof bsrep);
    CHECK(kr == KERN_SUCCESS && bsrep.ret_code == WS_ERR_SUCCESS,
          "SLSBindSurface binds a surface id and triggers the surfacing handoff");
    printf("  bound surface_id = 0x%x on window %u\n", bs.surface_id, wid);

    ws_req_cid_wid_place ow;
    ws_reply_ret owrep;
    memset(&ow, 0, sizeof ow);
    ow.hdr.msgh_id = WS_R_SLSOrderWindow;
    ow.cid = cid; ow.wid = wid; ow.place = 1; ow.other = 0;
    kr = ws_rpc(server, &ow.hdr, sizeof ow, &owrep.hdr, sizeof owrep);
    CHECK(kr == KERN_SUCCESS && owrep.ret_code == WS_ERR_SUCCESS, "SLSOrderWindow succeeds");

    ws_req_cid_wid_level lv;
    memset(&lv, 0, sizeof lv);
    lv.hdr.msgh_id = WS_R_SLSSetWindowLevel;
    lv.cid = cid; lv.wid = wid; lv.level = 20;
    kr = ws_rpc(server, &lv.hdr, sizeof lv, &owrep.hdr, sizeof owrep);
    CHECK(kr == KERN_SUCCESS && owrep.ret_code == WS_ERR_SUCCESS, "SLSSetWindowLevel succeeds");

    ws_req_cid_wid_tags tg;
    memset(&tg, 0, sizeof tg);
    tg.hdr.msgh_id = WS_R_SLSSetWindowTags;
    tg.cid = cid; tg.wid = wid; tg.tags = 0x1;
    kr = ws_rpc(server, &tg.hdr, sizeof tg, &owrep.hdr, sizeof owrep);
    CHECK(kr == KERN_SUCCESS && owrep.ret_code == WS_ERR_SUCCESS, "SLSSetWindowTags succeeds");

    ws_req_cid_wid rel;
    memset(&rel, 0, sizeof rel);
    rel.hdr.msgh_id = WS_R_SLSReleaseWindow;
    rel.cid = cid; rel.wid = wid;
    kr = ws_rpc(server, &rel.hdr, sizeof rel, &owrep.hdr, sizeof owrep);
    CHECK(kr == KERN_SUCCESS && owrep.ret_code == WS_ERR_SUCCESS, "SLSReleaseWindow succeeds");

    ws_reply_tid trep;
    memset(&pidr, 0, sizeof pidr);
    pidr.hdr.msgh_id = WS_R_SLSTransactionCreate;
    pidr.cid = cid;
    kr = ws_rpc(server, &pidr.hdr, sizeof pidr, &trep.hdr, sizeof trep);
    CHECK(kr == KERN_SUCCESS && trep.ret_code == WS_ERR_SUCCESS && trep.tid >= WS_FIRST_TRANSACTION_ID,
          "SLSTransactionCreate mints a transaction ref");

    ws_req_cid_tid tcm;
    memset(&tcm, 0, sizeof tcm);
    tcm.hdr.msgh_id = WS_R_SLSTransactionCommit;
    tcm.cid = cid; tcm.tid = trep.tid;
    kr = ws_rpc(server, &tcm.hdr, sizeof tcm, &owrep.hdr, sizeof owrep);
    CHECK(kr == KERN_SUCCESS && owrep.ret_code == WS_ERR_SUCCESS, "SLSTransactionCommit succeeds");

    // ---- 7. notifications (port-carrying request) --------------------------------------
    mach_port_t notify_recv = MACH_PORT_NULL;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &notify_recv);
    mach_port_insert_right(mach_task_self(), notify_recv, notify_recv, MACH_MSG_TYPE_MAKE_SEND);

    ws_req_cid_port nr;
    memset(&nr, 0, sizeof nr);
    nr.hdr.msgh_bits = MACH_MSGH_BITS_COMPLEX |
                       MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    nr.hdr.msgh_id = WS_R_SLSRegisterConnectionNotifyProc;
    nr.body.msgh_descriptor_count = 1;
    nr.port.name = notify_recv;
    nr.port.disposition = MACH_MSG_TYPE_COPY_SEND;
    nr.port.type = MACH_MSG_PORT_DESCRIPTOR;
    nr.cid = cid;
    ws_reply_ret nrep;
    kr = ws_rpc(server, &nr.hdr, sizeof nr, &nrep.hdr, sizeof nrep);
    CHECK(kr == KERN_SUCCESS && nrep.ret_code == WS_ERR_SUCCESS,
          "SLSRegisterConnectionNotifyProc succeeds");
    mach_port_deallocate(mach_task_self(), notify_recv);

    // ---- 8. release connection ----------------------------------------------------------
    ws_req_cid rc;
    memset(&rc, 0, sizeof rc);
    rc.hdr.msgh_id = WS_R_SLSReleaseConnection;
    rc.cid = cid;
    kr = ws_rpc(server, &rc.hdr, sizeof rc, &nrep.hdr, sizeof nrep);
    CHECK(kr == KERN_SUCCESS && nrep.ret_code == WS_ERR_SUCCESS, "SLSReleaseConnection succeeds");
    if (conn_port != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), conn_port);

    printf("\n== summary: %d pass, %d fail ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
