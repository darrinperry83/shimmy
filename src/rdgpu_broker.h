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

/* Host-only fixture entry. Production accepts no peer until a signed shim
 * attests the guest process and authenticates the broker connection. Never
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
