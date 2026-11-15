// windowserver.h — shim-as-WindowServer stand-in (first cut).
//
// This module is the *control plane* half of replacing WindowServer: it checks the
// two SkyLight bootstrap services in (`com.apple.windowserver`, then
// `com.apple.windowserver.active` — lowercase, that order, exactly as
// WINDOWSERVER_BOOTSTRAP_REGISTRATION.md §3.2 records) and serves a MIG-style CGS/SLS
// message loop that answers the ~37-function surface the background services
// (Dock / SystemUIServer / loginwindow / NotificationCenter) need at startup.
//
// It does NOT implement the pixel half (CGWindow -> IOSurface -> Wayland); that is a
// follow-up. It does NOT take over the live WindowServer. In the default (test) mode it
// registers a *test* service name with `bootstrap_register` so the check-in + handshake
// can be proven headlessly without a launchd plist and without touching the real
// `com.apple.windowserver` name. The real-name `bootstrap_check_in` path exists but is
// gated behind `RDNATIVE_WINDOWSERVER_SYSTEM=1` + root and is never used in validation.
//
// Protocol: a self-consistent, MIG-shaped Mach protocol (see "Wire format" below).
// It follows the MIG conventions the real SkyLight transport uses where they are known —
// request `msgh_id = routine id`, reply `msgh_id = routine id + 100`, a send-once reply
// port, `MACH_MSGH_BITS_COMPLEX` + a port descriptor for any reply/request that moves a
// Mach port — but the routine-number table is a *stand-in* table, not the real SkyLight
// op-code table (which the app-lifecycle report §7 still lists as an open item). When the
// real table is reverse-engineered, the dispatch table below is the only thing to swap.
//
//   Request (client -> server):
//     mach_msg_header_t  hdr        msgh_id = routine id
//                                   msgh_bits = MACH_MSGH_BITS(COPY_SEND, MAKE_SEND_ONCE)
//                                   msgh_remote_port = server port
//                                   msgh_local_port  = client reply (receive right)
//     [ mach_msg_body_t + mach_msg_port_descriptor_t ]   only when the call passes a port
//     ...routine-specific in-args (fixed structs below)...
//
//   Reply (server -> client):
//     mach_msg_header_t  hdr        msgh_id = routine id + 100
//                                   msgh_bits = MACH_MSGH_BITS(REPLY(req bits), 0)
//                                   msgh_remote_port = request's reply port
//     [ mach_msg_body_t + mach_msg_port_descriptor_t ]   only when the call returns a port
//     int32_t ret_code             0 = success; negative = CGError
//     ...routine-specific out-args (fixed structs below)...
//
// (The NDR_record MIG inserts between the header and RetCode is omitted on purpose: both
// ends are arm64 little-endian and co-compiled, so it carries no information here.)
#ifndef RDNATIVE_WINDOWSERVER_H
#define RDNATIVE_WINDOWSERVER_H

#include <mach/mach.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- service names (the two SkyLight checks in, mirroring the bootstrap report) --------
#define WS_REAL_SERVICE_NAME        "com.apple.windowserver"
#define WS_REAL_ACTIVE_NAME         "com.apple.windowserver.active"
#define WS_TEST_SERVICE_NAME        "com.shimmy.windowserver.test"
#define WS_TEST_ACTIVE_NAME         "com.shimmy.windowserver.active.test"

// ---- fixed identity values (coherent defaults for the stand-in) ------------------------
#define WS_MAIN_CONNECTION_ID       0x3E8u        // 1000 — per the task's pin
#define WS_FIRST_MINTED_CID         0x3E9u        // SLSNewConnection mints 0x3E9, 0x3EA, ...
#define WS_MAIN_DISPLAY_ID          1u
#define WS_DISPLAY_COUNT            1u            // one non-empty display list
#define WS_MAX_DISPLAYS             16u
#define WS_DISPLAY_WIDTH            1920
#define WS_DISPLAY_HEIGHT           1080
#define WS_DISPLAY_REFRESH_HZ       60u
#define WS_DISPLAY_DEPTH            32u
#define WS_FIRST_WINDOW_ID          100u
#define WS_FIRST_TRANSACTION_ID     1u
#define WS_SERVER_VERSION           0x00010000u   // pidversion handed back to GetPIDAndVersion
#define WS_RECV_TIMEOUT_MS          100u          // server-loop idle tick: lets a warm-switch
                                                  // signal be observed promptly (see ws_server_run)

// ---- CGError values the stand-in returns (subset) --------------------------------------
#define WS_ERR_SUCCESS              0
#define WS_ERR_FAILURE              1000
#define WS_ERR_ILLEGAL_ARGUMENT     1001
#define WS_ERR_INVALID_CONNECTION   1002
#define WS_ERR_INVALID_WINDOW       1003
#define WS_ERR_NOT_FOUND            1004

// ---- scalar types (the CGS/SLS opaque ids, sized for the wire) -------------------------
typedef uint32_t ws_cid_t;      // CGSConnectionID
typedef uint32_t ws_wid_t;      // CGWindowID
typedef uint32_t ws_did_t;      // CGDirectDisplayID
typedef uint32_t ws_tid_t;      // transaction id (stand-in opaque ref)

// ---- fixed records ---------------------------------------------------------------------
typedef struct { int32_t x, y, w, h; } ws_rect_t;              // points, origin top-left

typedef struct {                                                 // stand-in display mode
    uint32_t width;
    uint32_t height;
    uint32_t refresh_hz;
    uint32_t depth;
    uint32_t flags;
} ws_display_mode_t;

typedef struct { uint8_t b[16]; } ws_uuid_t;                    // CFUUIDBytes

// Stand-in for SLSSessionCopyCurrentSessionProperties' CFDictionary. Real enough for the
// services to see "a valid session dictionary"; the dictionary/XPC transport that would
// carry a true CFDictionary is a follow-up.
typedef struct {
    uint32_t user_id;       // uid of the console user (501 on the reference machine)
    uint32_t session_id;    // login session id
    uint32_t on_console;    // 1
    uint32_t locked;        // 0
} ws_session_props_t;

#define WS_PROP_KEY_MAX   64
#define WS_PROP_VAL_MAX   128
#define WS_LOGIN_NAME_MAX 256

// ---- routine ids (the ~37-function surface, grouped per CGS_BACKGROUND_SERVICES_SURFACE.md §4)
// Connection bootstrap (6)
#define WS_R_SLSMainConnectionID                                        1000
#define WS_R_SLSNewConnection                                           1001
#define WS_R_SLSServerPort                                              1002
#define WS_R_SLSReleaseConnection                                       1003
#define WS_R_SLSConnectionGetPIDAndVersion                              1004
#define WS_R_SLSGetConnectionIDForPSN                                   1005
// Session + identity (10)
#define WS_R_SLSSessionCopyCurrentSessionProperties                     1006
#define WS_R_SLSSessionCopyAllSessionProperties                         1007
#define WS_R_SLSSessionSetCurrentSessionProperties                      1008
#define WS_R_SLSSetLoginwindowConnection                                1009
#define WS_R_SLSSetLoginName                                            1010
#define WS_R_SLSSessionSwitchToSessionID                                1011
#define WS_R_SLSSetConnectionProperty                                   1012
#define WS_R_SLSCopyConnectionProperty                                  1013
#define WS_R_SLSSessionSetCurrentSessionWorkspaceNotificationConnectionID 1014
#define WS_R_SLSSessionSetCurrentSessionWorkspacePreferences            1015
// Notifications (4)
#define WS_R_SLSRegisterConnectionNotifyProc                            1016
#define WS_R_SLSRemoveConnectionNotifyProc                              1017
#define WS_R_SLSRegisterNotifyProc                                      1018
#define WS_R_SLSRemoveNotifyProc                                        1019
// Events (1)
#define WS_R_SLSGetEventPort                                            1020
// Display enumeration (8)
#define WS_R_SLSMainDisplayID                                           1021
#define WS_R_SLSGetDisplayList                                          1022
#define WS_R_SLSGetDisplayBounds                                        1023
#define WS_R_SLSGetDisplayForUUID                                       1024
#define WS_R_SLSCopyDisplayUUID                                         1025
#define WS_R_SLSGetCurrentDisplayMode                                   1026
#define WS_R_SLSGetPotentiallyActiveDisplayList                         1027
#define WS_R_SLSCopyActiveMenuBarDisplayIdentifier                      1028
// Window + transaction stub layer (8)
#define WS_R_SLSNewWindowWithOpaqueShape                                1029
#define WS_R_SLSNewWindowWithOpaqueShapeAndContext                      1030
#define WS_R_SLSOrderWindow                                             1031
#define WS_R_SLSSetWindowLevel                                          1032
#define WS_R_SLSSetWindowTags                                           1033
#define WS_R_SLSReleaseWindow                                           1034
#define WS_R_SLSTransactionCreate                                       1035
#define WS_R_SLSTransactionCommit                                       1036
// Window + surface bind (1) — the SLSBindSurface -> surfacing handoff. When a mac app
// registers its window (SLSNewWindow...) and binds its IOSurface (SLSBindSurface), the shim
// hands (window_id, surface_id) to ws_mac_surface_present, which drives the existing
// mac-app surfacing path (wlsrv.c mac_surface_*): the window surfaces as a native Wayland
// window in Hyprland.
#define WS_R_SLSBindSurface                                             1037

#define WS_R_FIRST   WS_R_SLSMainConnectionID
#define WS_R_LAST    WS_R_SLSBindSurface
#define WS_R_COUNT   (WS_R_LAST - WS_R_FIRST + 1)     // must be 38

// ---- request structs (the in-arg portion appended after the header) --------------------
typedef struct { mach_msg_header_t hdr; } ws_req_empty;                          // no in-args
typedef struct { mach_msg_header_t hdr; uint32_t options; } ws_req_newconn;     // SLSNewConnection

typedef struct { mach_msg_header_t hdr; uint32_t cid; } ws_req_cid;
typedef struct { mach_msg_header_t hdr; uint32_t cid; ws_rect_t rect; } ws_req_cid_rect;
typedef struct { mach_msg_header_t hdr; uint32_t did; } ws_req_did;
typedef struct { mach_msg_header_t hdr; ws_uuid_t uuid; } ws_req_uuid;
typedef struct { mach_msg_header_t hdr; uint32_t sid; } ws_req_sid;
typedef struct { mach_msg_header_t hdr; uint32_t cid; char key[WS_PROP_KEY_MAX]; } ws_req_cid_key;
typedef struct {
    mach_msg_header_t hdr;
    uint32_t cid;
    char key[WS_PROP_KEY_MAX];
    uint32_t vlen;                       // <= WS_PROP_VAL_MAX
    uint8_t val[WS_PROP_VAL_MAX];
} ws_req_cid_key_val;
typedef struct {
    mach_msg_header_t hdr;
    uint32_t cid;
    char name[WS_LOGIN_NAME_MAX];
} ws_req_cid_name;
typedef struct { mach_msg_header_t hdr; uint32_t cid; uint32_t wid; } ws_req_cid_wid;
typedef struct {
    mach_msg_header_t hdr;
    uint32_t cid;
    uint32_t wid;
    int32_t level;
} ws_req_cid_wid_level;
typedef struct {
    mach_msg_header_t hdr;
    uint32_t cid;
    uint32_t wid;
    uint64_t tags;
} ws_req_cid_wid_tags;
typedef struct {
    mach_msg_header_t hdr;
    uint32_t cid;
    uint32_t wid;
    uint32_t place;
    uint32_t other;
} ws_req_cid_wid_place;
typedef struct {                                                    // SLSBindSurface (stand-in table)
    mach_msg_header_t hdr;
    uint32_t cid;
    uint32_t wid;
    uint32_t surface_id;    // bound IOSurface id (IOSurfaceGetID); synthetic in the headless peer
    uint32_t type;          // kCGSWindowSurfaceType_* (opaque to the stand-in)
    int32_t  w, h;          // surface pixel size (informational)
} ws_req_cid_wid_surface;
typedef struct { mach_msg_header_t hdr; uint32_t cid; uint32_t tid; } ws_req_cid_tid;
typedef struct { mach_msg_header_t hdr; uint32_t psn_hi; uint32_t psn_lo; } ws_req_psn;
// Complex request: carries one send right (a notification / event registration port).
typedef struct {
    mach_msg_header_t hdr;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port;
    uint32_t cid;
} ws_req_cid_port;

// ---- reply structs (the out-arg portion; every reply starts with header + ret_code) ----
typedef struct { mach_msg_header_t hdr; int32_t ret_code; } ws_reply_ret;
typedef struct { mach_msg_header_t hdr; int32_t ret_code; uint32_t cid; } ws_reply_cid;
typedef struct { mach_msg_header_t hdr; int32_t ret_code; uint32_t pid; uint32_t version; } ws_reply_pidver;
typedef struct { mach_msg_header_t hdr; int32_t ret_code; uint32_t did; } ws_reply_did;
typedef struct {
    mach_msg_header_t hdr;
    int32_t ret_code;
    uint32_t count;                       // 1..WS_MAX_DISPLAYS
    uint32_t dids[WS_MAX_DISPLAYS];
} ws_reply_didlist;
typedef struct { mach_msg_header_t hdr; int32_t ret_code; ws_rect_t rect; } ws_reply_rect;
typedef struct { mach_msg_header_t hdr; int32_t ret_code; ws_uuid_t uuid; } ws_reply_uuid;
typedef struct { mach_msg_header_t hdr; int32_t ret_code; ws_display_mode_t mode; } ws_reply_mode;
typedef struct { mach_msg_header_t hdr; int32_t ret_code; ws_session_props_t props; } ws_reply_session;
typedef struct {
    mach_msg_header_t hdr;
    int32_t ret_code;
    uint32_t vlen;
    uint8_t val[WS_PROP_VAL_MAX];
} ws_reply_propval;
typedef struct { mach_msg_header_t hdr; int32_t ret_code; uint32_t wid; } ws_reply_wid;
typedef struct { mach_msg_header_t hdr; int32_t ret_code; uint32_t tid; } ws_reply_tid;
// Complex replies: return one send right (the connection/event/server port).
typedef struct {
    mach_msg_header_t hdr;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port;
    int32_t ret_code;
    uint32_t cid;
} ws_reply_newconn;                       // SLSNewConnection -> (cid, connection port)
typedef struct {
    mach_msg_header_t hdr;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port;
    int32_t ret_code;
} ws_reply_port;                          // SLSServerPort / SLSGetEventPort -> a port

// ============================================================================
// REAL SkyLight SLS MIG wire protocol (decoded from the SkyLight shared-cache
// disassembly, macOS 26.5.1 / 25F80 arm64e). This is the protocol a REAL
// CoreGraphics/AppKit client speaks, layered over the SAME Mach transport as the
// stand-in table above. The two differ only in (a) msgh_id (real SLS routine ids,
// not 1000-1036), (b) reply msgh_id = request + 0x64 (not +100), and (c) a MIG
// NDR_record between the header and the ret code / in-args.
//
// Request (client -> server):
//   mach_msg_header_t  hdr     msgh_bits = 0x1513 (or 0x80001513 if COMPLEX)
//                              msgh_id   = SLS routine id (below)
//                              msgh_remote_port = per-connection server send right
//                              msgh_local_port  = reply port (send-once)
//   uint64_t tok;              offset 0x18, opaque connection-context token
//   ...routine in-args at offset 0x20...
//
// Reply (server -> client):
//   mach_msg_header_t  hdr     msgh_id = request + 0x64 (or 0x47 for a rejected call)
//   uint64_t ndr;              offset 0x18, MIG NDR_record (arm64 LE)
//   int32_t  ret_code;         offset 0x20, 0 = success
//   ...routine out-args at offset 0x24...
//
// SLSNewConnection is the one COMPLEX reply: header + body(count=2) + two port
// descriptors (MOVE_SEND) + NDR + the minted cid.
// ============================================================================
#define SLS_REPLY_OFFSET    0x64u                 // reply id = request id + 0x64
#define SLS_REPLY_ERROR_ID  0x47u                 // error reply id
#define SLS_REAL_MAGIC      0x1513u               // MACH_MSGH_BITS(COPY_SEND, MAKE_SEND_ONCE)
#define SLS_REAL_MAGIC_CX   0x80001513u           // ... | COMPLEX
#define SLS_NDR             0x0000000100000000ULL // NDR_record bytes (arm64 LE)

// Real SLS routine ids (request msgh_id). Decoded from the 8-byte envelope
// constants {voucher=0, msgh_id} in SkyLight __const AND cross-checked against the
// __DATA_CONST routine-descriptor table (name-string pointer, msgh_id pairs at
// 0x1e5e67de0..0x1e5e6a14x; name strings at 0x18720xxxx..0x18721xxxx). Reply id =
// request + 0x64 everywhere (the marshal validates it, e.g. `cmp w8, #0x759e` for
// OrderWindow 0x753a, `cmp w8, #0x769b` for CreateWindow 0x7637, `cmp w8, #0x7742`
// for TransactionCommitSync 0x76de).
#define SLS_R_NewConnection              0x7d00u   // "NewConnectionPort"
#define SLS_R_ConnectionGetPIDAndVersion 0x7d01u   // "ConnectionGetPIDAndVersion"
// SLSNewWindowWithOpaqueShape / ...AndContext are NOT distinct RPCs: they delegate to
// the single "CreateWindow" routine (0x7637) with an operation code (0xd) in the body.
// 0x74d4/0x74d8 appear nowhere in the const or routine tables — they were wrong.
#define SLS_R_NewWindowWithOpaqueShape   0x7637u   // "CreateWindow"
// SLSBindSurface's REAL SLS MIG routine id (msgh_id), decoded from the 8-byte envelope
// constant {voucher=0, msgh_id} at SkyLight __TEXT 0x1871b2630 (msgh_id word at 0x1871b2634),
// loaded by the SLSBindSurface MIG marshaller at SkyLight+0xf398 (0x186ce8398) via
//   adrp x8, #0x1871b2000 ; ldr d0, [x8, #0x630]
// and cross-checked against the __DATA_CONST routine-descriptor table entry at 0x1e5e68268.
// The client validates the reply msgh_id == 0x75f1 (cmp w8, #0x75f1 at SkyLight+0xf500),
// i.e. reply = 0x758d + 0x64. The reply shape (msgh_id = request + 0x64, NDR + ret_code)
// is shared with every other real routine.
#define SLS_R_BindSurface                0x758du   // "BindSurface"
// SLSOrderWindow is NOT "OrderWindowList with count=1" at the RPC layer. The C wrapper
// SLSOrderWindow (0x187002a94) does call SLSOrderWindowList (0x1870027a8) with count=1,
// but OrderWindowList special-cases count==1 and routes it to the SINGLE-window marshal
// 0x186ce6ba8, which sends the DISTINCT "OrderWindow" routine:
//   adrp x8,#0x1871b2000 ; ldr d0,[x8,#0x5c8]  -> const 0x1871b25c8 = {0, 0x753a}
//   reply validated with cmp w8,#0x759e (0x753a+0x64).
// The count>1 path sends "OrderWindowList" 0x753b (const 0x1871b1db8, reply 0x759f).
#define SLS_R_OrderWindow                0x753au   // "OrderWindow" (single)
#define SLS_R_OrderWindowList            0x753bu   // "OrderWindowList"
#define SLS_R_GetWindowList              0x7573u   // "GetWindowList"
// SLSReleaseWindow maps to the "TerminateWindow" routine (0x7535). Its marshal materialises
// the id inline: `mov w8,#0x7535 ; stp wzr,w8,[sp,#0x10]` at 0x186ff1008, and expects a
// 0x24-byte reply (header+NDR+ret_code). There is NO "ReleaseWindow"/"GetEventPort" routine
// in the descriptor table — GetEventPort is a LOCAL connection-cache read (SkyLight+0x34aeac
// reads the event port from the connection object at +0x18; no Mach round-trip).
#define SLS_R_ReleaseWindow              0x7535u   // "TerminateWindow"
#define SLS_R_TransactionCommit          0x76deu   // "TransactionCommitSync"
#define SLS_R_TransactionCommitAsync     0x76ddu   // "TransactionCommitAsync"
#define SLS_R_GetDisplayForUUID          0x7669u

#define SLS_REAL_ID_MIN  0x7400u
#define SLS_REAL_ID_MAX  0x7e00u

#define WS_MAX_WINDOWS 256

// Real-protocol request structs (8-byte token sits between header and in-args).
typedef struct { mach_msg_header_t hdr; uint64_t tok; } sls_req_hdr;
typedef struct { mach_msg_header_t hdr; uint64_t tok; uint32_t cid; } sls_req_cid;
typedef struct { mach_msg_header_t hdr; uint64_t tok; uint32_t cid; uint32_t max; } sls_req_cid_max;
// SLSOrderWindow (0x753a): single-window order. Three u32 in-args after the token.
typedef struct {
    mach_msg_header_t hdr;
    uint64_t tok;               // offset 0x18, connection-context token
    uint32_t wid;               // offset 0x20, CGWindowID
    uint32_t place;             // offset 0x24, order mode (kCGSOrderAbove/etc.)
    uint32_t other;             // offset 0x28, relative window id
} sls_req_orderwindow;          // request size 0x2c
// SLSReleaseWindow -> TerminateWindow (0x7535). One u32 in-arg after the token.
typedef struct {
    mach_msg_header_t hdr;
    uint64_t tok;               // offset 0x18, connection-context token
    uint32_t wid;               // offset 0x20, CGWindowID to destroy
} sls_req_terminatewindow;      // request size 0x24; reply = header+NDR+ret_code (0x24)
// SLSNewWindowWithOpaqueShape(+AndContext) -> CreateWindow (0x7637). The connection is the
// token + the port the message is sent to; the body is an opaque window-params blob whose
// first u32 is the operation code (0xd for the classic SLSNewWindow path).
typedef struct {
    mach_msg_header_t hdr;
    uint64_t tok;               // offset 0x18, connection-context token
    uint32_t op;                // offset 0x20, operation code (0xd = create window)
} sls_req_createwindow;
// SLSTransactionCommit -> TransactionCommitSync (0x76de) / Async (0x76dd). The stand-in
// treats the transaction payload as opaque; only the token is fixed on the wire.
typedef struct {
    mach_msg_header_t hdr;
    uint64_t tok;               // offset 0x18, connection-context token
    uint32_t tid;               // offset 0x20, transaction id (stand-in ref)
} sls_req_transaction;
typedef struct {                                                    // SLSBindSurface (real protocol)
    mach_msg_header_t hdr;      // msgh_bits=0x1513, msgh_id=0x758d
    uint64_t tok;               // offset 0x18, connection-context token
    uint32_t wid;               // offset 0x20, CGWindowID
    uint32_t surface_id;        // offset 0x24, bound IOSurface id (IOSurfaceGetID)
    uint32_t type;              // offset 0x28, kCGSWindowSurfaceType_* (opaque to the stand-in)
    int32_t  w;                 // offset 0x2c, surface pixel width  (informational)
    int32_t  h;                 // offset 0x30, surface pixel height (informational)
} sls_req_bindsurface;          // request size 0x34; NO connection id on the wire — the
                                // connection is identified by the port the message is sent to.

// Real-protocol reply structs (NDR + ret_code precede the out-args).
typedef struct { mach_msg_header_t hdr; uint64_t ndr; int32_t ret_code; } sls_reply_ret;
typedef struct { mach_msg_header_t hdr; uint64_t ndr; int32_t ret_code; uint32_t cid; } sls_reply_cid;
typedef struct { mach_msg_header_t hdr; uint64_t ndr; int32_t ret_code; uint32_t pid; uint32_t version; } sls_reply_pidver;
typedef struct { mach_msg_header_t hdr; uint64_t ndr; int32_t ret_code; uint32_t count; uint32_t wids[WS_MAX_WINDOWS]; } sls_reply_widlist;

// The SLSNewConnection COMPLEX reply (header + body(2) + two MOVE_SEND ports + NDR + cid).
typedef struct {
    mach_msg_header_t hdr;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port0;
    mach_msg_port_descriptor_t port1;
    uint64_t ndr;
    uint32_t cid;
} sls_reply_newconn;

// ---- public entry points ---------------------------------------------------------------
// Check the two real services in with bootstrap_check_in, mirroring the report order
// (main, then .active). Only callable as root under a launchd plist that owns those names;
// never used by the validation run.
kern_return_t ws_bootstrap_check_in_system(mach_port_t bootstrap_port,
                                           mach_port_t *main_out,
                                           mach_port_t *active_out);

// Register test service names with bootstrap_register (allocate receive right + register).
// This is the safe, headless path the validation peer uses. Returns 0 on success.
kern_return_t ws_bootstrap_register_test(mach_port_t bootstrap_port,
                                         const char *service_name,
                                         const char *active_name,
                                         mach_port_t *main_out,
                                         mach_port_t *active_out);

// Run the CGS/SLS server loop on `main_port` until a signal. Returns the process exit code.
int ws_server_run(mach_port_t main_port);

// Serve the stand-in on `main_port`: set up the send right + the receive port set
// (main + every per-connection port), then run the receive loop. This is the single
// entry the injected-payload host needs — it does the same setup main() does AFTER the
// bootstrap check-in, without re-running main()'s check-in/register (the injector or the
// payload owns that). Returns ws_server_run's exit code, or 1 on a null port.
int ws_server_serve(mach_port_t main_port);

// Human-readable name of a routine id (for the server log + the peer report).
const char *ws_routine_name(uint32_t routine_id);

// Surfacing handoff, invoked by SLSBindSurface once a window has a bound IOSurface.
// window_id is the server-minted CGWindowID; surface_id is the bound IOSurface id.
// The windowserver stand-in provides a WEAK default that logs the would-be wl_surface
// creation (the headless wiring proof). The surfacing host (wlsrv.c mac_surface_*)
// overrides it with the STRONG implementation that actually creates the Hyprland-owned
// wl_surface + xdg_toplevel and presents the surface. When the two are linked into one
// image, the strong symbol wins.
void ws_mac_surface_present(uint32_t window_id, uint32_t surface_id);

#ifdef __cplusplus
}
#endif
#endif // RDNATIVE_WINDOWSERVER_H
