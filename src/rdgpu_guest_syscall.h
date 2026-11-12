#ifndef RDGPU_GUEST_SYSCALL_H
#define RDGPU_GUEST_SYSCALL_H
#include "rdgpu_wire.h"
#include "rdgpu_draw_batch_wire.h"
#include "rdgpu_export_token.h"
#include <pthread.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>
#define RDGPU_GUEST_SYSCALL 0x2002u
#define RDGPU_GUEST_DRAW_BATCH 6u
#define RDGPU_GUEST_DRAW_POLL 7u
#define RDGPU_GUEST_FD_EXPORT 11u
#define RDGPU_GUEST_FD_IMPORT 12u
#define RDGPU_GUEST_IMAGE_STRIDE 13u
typedef long (*RDGPUImageStrideQuery)(void *context,pid_t attestedGuestPid,
                                     const uint8_t lease[16],uint64_t generation,
                                     uint32_t width,uint32_t height);
/* The issuer runs in the trusted host shim after exact slot lookup. It must
 * return a CLOEXEC FD for this lease or a negative errno; the guest cannot
 * supply or mint the FD. */
typedef long (*RDGPUFDExportIssue)(void *context,pid_t attestedGuestPid,
                                   const uint8_t lease[16],uint64_t generation,
                                   uint64_t handle,uint64_t completedFence,
                                   uint32_t width,uint32_t height,uint32_t stride);
/* Resolve a received dma-buf carrier fd back to its broker lease. The host
 * shim matches the fd's fstat(st_dev, st_ino) against each issued carrier;
 * the guest slot table then maps the lease back to the original opaque handle
 * so the driver reuses the SAME IOSurface-backed resource. Returns 0 on
 * success (lease_out filled) or a negative errno. */
typedef int (*RDGPUFDImportResolve)(void *context,pid_t attestedGuestPid,
                                    int fd,uint8_t lease_out[16]);
typedef RDGPUReply (*RDGPUTransport)(void *context,const RDGPURequest *request);
/* Positive session-local completion fence or negative errno. */
typedef long (*RDGPUDrawBatchTransport)(void *context,
                                      const RDGPUDrawBatchRequest *request,
                                      const void *snapshot,size_t bytes);
typedef int (*RDGPUDrawPollTransport)(void *context,uint64_t fence);
typedef int (*RDGPUExportAuthorize)(void *context,pid_t attestedGuestPid,
                                    const uint8_t lease[16],uint64_t generation);
/* Only a trusted signed host broker can issue the token. It atomically
 * prepares the export and returns a validated reply plus bound image record. */
typedef int (*RDGPUExportIssue)(void *context,pid_t attestedGuestPid,
                                const RDGPURequest *request,RDGPUReply *reply,
                                RDGPUExportImage *image);
typedef void (*RDGPUExportAbort)(void *context,pid_t attestedGuestPid,
                                 const uint8_t token[16],uint64_t sequence);
/* The caller MUST check this whole range against a trusted ledger of
 * writable guest-owned mappings. A Mach VM region/protection check alone
 * cannot distinguish shim memory from guest memory in this process. */
typedef int (*RDGPUExportCopyout)(void *context,uint64_t guestAddress,
                                  const void *bytes,size_t length);
typedef struct {
    uint64_t handle,generation,exportHandle,brokerReceipt;
    uint32_t width,height;
    uint8_t lease[16];
    uint64_t drawFence;
    uint64_t lastCompletedFence;
    int producerComplete,exported;
} RDGPUGuestSlot;
typedef struct {
    pthread_mutex_t lock;
    pid_t ownerPid;
    uint64_t nextRequest;
    RDGPUTransport transport;
    RDGPUDrawBatchTransport drawTransport;
    RDGPUDrawPollTransport drawPollTransport;
    void *drawContext;
    void *context;
    RDGPUExportAuthorize exportAuthorize;
    void *exportContext;
    RDGPUExportIssue exportIssue;
    RDGPUExportAbort exportAbort;
    RDGPUExportCopyout exportCopyout;
    RDGPUFDExportIssue fdExportIssue;
    void *fdExportContext;
    RDGPUFDImportResolve fdImportResolve;
    void *fdImportContext;
    RDGPUImageStrideQuery imageStrideQuery;
    void *imageStrideContext;
    void *exportTransportContext;
    int ready,disabled;
    RDGPUGuestSlot slots[RDGPU_MAX_IMAGES_PER_PEER];
} RDGPUGuestState;
void rdgpu_guest_state_init(RDGPUGuestState *state,pid_t attestedPid,
                            RDGPUTransport transport,void *context);
void rdgpu_guest_set_export_authorizer(RDGPUGuestState *state,
                                      RDGPUExportAuthorize authorize,
                                      void *context);
void rdgpu_guest_set_export_transport(RDGPUGuestState *state,
                                     RDGPUExportIssue issue,
                                     RDGPUExportAbort abortExport,
                                     RDGPUExportCopyout copyout,
                                     void *context);
void rdgpu_guest_set_draw_transport(RDGPUGuestState *state,
                                   RDGPUDrawBatchTransport transport,
                                   RDGPUDrawPollTransport poll,
                                   void *context);
void rdgpu_guest_set_fd_export_transport(RDGPUGuestState *state,
                                        RDGPUFDExportIssue issue,void *context);
void rdgpu_guest_set_fd_import_transport(RDGPUGuestState *state,
                                         RDGPUFDImportResolve resolve,void *context);
void rdgpu_guest_set_image_stride_transport(RDGPUGuestState *state,
                                            RDGPUImageStrideQuery query,void *context);
long rdgpu_guest_syscall(RDGPUGuestState *state,uint64_t op,uint64_t a1,
                          uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5);
#endif
