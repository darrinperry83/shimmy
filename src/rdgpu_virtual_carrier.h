#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

#define RDCARRIER_ARGB8888 0x34325241u /* DRM fourcc AR24 */
typedef struct {
    uint32_t width,height,stride,fourcc;
    uint64_t modifier;
} RDCarrierFormat;
/* Host capability issued by the separate rdcap table for this exact lease.
 * It is never put in the guest FD's bytes or sent as a Wayland argument. */
typedef struct {uint64_t opaque,generation;} RDCarrierCapability;
typedef struct RDCarrier RDCarrier;
typedef void (*RDCarrierReturn)(void *context,const uint8_t lease[16],
                                uint64_t generation);

/* Host-only test table. The lease is never encoded in the FD's bytes. */
RDCarrier *rd_carrier_create(pid_t compositor_pid,uint64_t audit_session,
                            RDCarrierReturn on_return,void *context);
bool rd_carrier_issue(RDCarrier *table,const uint8_t lease[16],
                      uint64_t generation,RDCarrierFormat format,
                      uint64_t producer_fence,RDCarrierCapability capability,
                      int *carrier_fd);
bool rd_carrier_complete(RDCarrier *table,const uint8_t lease[16],
                         uint64_t generation,uint64_t completed_fence);
/* fd and metadata come from the protocol; pid/session/client must come from
 * a trusted compositor's OS identity/object table, never guest fields. */
bool rd_carrier_import(RDCarrier *table,int fd,RDCarrierFormat format,
                       uint64_t sequence,pid_t compositor_pid,
                       uint64_t audit_session,const void *wl_client,
                       const void *wl_buffer,RDCarrierCapability *out);
/* True when the table has an issued carrier whose recorded (st_dev, st_ino)
 * equals fstat(fd). Mirrors the identity check inside rd_carrier_import but
 * does NOT consume/bind the carrier, so it is safe for a read-only fd->image
 * lookup. */
bool rd_carrier_matches_fd(RDCarrier *table,int fd);
bool rd_carrier_release(RDCarrier *table,const void *wl_client,
                        const void *wl_buffer);
void rd_carrier_destroy(RDCarrier *table);
#ifdef __cplusplus
}
#endif
