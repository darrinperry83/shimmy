#ifndef RDGPU_BROKER_H
#define RDGPU_BROKER_H
#include "rdgpu_wire.h"
#include "rdmetal_resource.h"
#include <stdbool.h>
#include <sys/types.h>

typedef struct RDGPUSession RDGPUSession;
typedef bool (*RDGPUVerifyTestPeer)(pid_t actualPeer, void *context);
/* Test-only host callback. Called with the session lock held and a live but
 * unretained surface. It must not re-enter this session. A nonzero receipt
 * means the callback retained or otherwise safely owns the export. */
typedef uint64_t (*RDGPUAuthorizeTestExport)(void *context,pid_t actualPeer,
                                              IOSurfaceRef surface,
                                              const uint8_t lease[16],
                                              uint64_t generation);
typedef void (*RDGPURevokeTestExport)(void *context,uint64_t receipt);

/* Same-process broker entry. The host shim obtains its peer identity from
 * getpid(); no guest-provided PID or unauthenticated socket is accepted. */
RDGPUSession *rdgpu_local_session_create(void);
/* Host-only import after the caller has authenticated a transferred surface.
 * The broker creates an independent lease in this process for the exact
 * IOSurface; guest-supplied ids or file paths must never call this directly. */
bool rdgpu_local_import_surface(RDGPUSession *session, IOSurfaceRef surface,
                                uint8_t lease_out[16], uint64_t *generation_out);
/* Metadata query is available before first render; it grants no image access. */
bool rdgpu_local_image_geometry(RDGPUSession *session,
                                const uint8_t lease[16],uint64_t generation,
                                uint32_t *width,uint32_t *height,uint32_t *stride);
/* Synchronous local draw only: the caller holds its guest slot lock until
 * draw_execute returns and calls draw_complete. The returned surface is
 * retained for draw_execute, which releases it. It is not an export grant. */
IOSurfaceRef rdgpu_local_draw_surface(RDGPUSession *session,
                                      const uint8_t lease[16],uint64_t generation);
/* A completed image may be sampled as an input by a synchronous draw in the
 * same attested guest session. The caller releases the returned CF reference. */
IOSurfaceRef rdgpu_local_sample_surface(RDGPUSession *session,
                                        const uint8_t lease[16],uint64_t generation);
bool rdgpu_local_draw_complete(RDGPUSession *session,
                               const uint8_t lease[16],uint64_t generation);
/* Synchronous upload into an EXISTING broker-owned BGRA8 IOSurface. The source
 * is a checked guest snapshot; no new image or export right is minted. */
int rdgpu_local_upload_image(RDGPUSession *session,
                             const uint8_t lease[16],uint64_t generation,
                             uint32_t source_format,const void *pixels,
                             size_t bytes);
int rdgpu_local_upload_image_region(RDGPUSession *session,
                             const uint8_t lease[16],uint64_t generation,
                             uint32_t source_format,const void *pixels,
                             size_t bytes,uint32_t x,uint32_t y,
                             uint32_t width,uint32_t height,uint32_t source_stride);

/* Host-only fixture entry. A broker in a different process still requires
 * signed-shim attestation and a kernel-authenticated connection. Never
 * construct a session using a guest-supplied PID. */
RDGPUSession *rdgpu_test_session_create(pid_t actualPeer,
                                        RDGPUVerifyTestPeer verify, void *context);
void rdgpu_test_set_export_callbacks(RDGPUSession *session,
                                     RDGPUAuthorizeTestExport authorize,
                                     RDGPURevokeTestExport revoke,void *context);
bool rdgpu_test_return_export(RDGPUSession *session,const uint8_t lease[16],
                              uint64_t generation,uint64_t receipt);
RDGPUReply rdgpu_dispatch(RDGPUSession *session, const RDGPURequest *request);
void rdgpu_session_disconnect(RDGPUSession *session);
/* Test-only retained export. The exact lease must have a completed producer
 * fence. The matching return releases the retained IOSurface reference; the
 * guest cannot release the image while an export is outstanding. */
IOSurfaceRef rdgpu_test_export_surface(RDGPUSession *session,
                                      const uint8_t lease[16],uint64_t generation);
bool rdgpu_test_return_surface(RDGPUSession *session,const uint8_t lease[16],
                               uint64_t generation,IOSurfaceRef surface);
#endif
