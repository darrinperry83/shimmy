#ifndef RDGPU_DRAW_BATCH_WIRE_H
#define RDGPU_DRAW_BATCH_WIRE_H
#include <stdint.h>
#define RDGPU_DRAW_BATCH_MAGIC 0x48435442u
#define RDGPU_DRAW_BATCH_VERSION 1u
#define RDGPU_DRAW_BATCH_MAX_BYTES (2u*16384u*4u+262144u+64u)
typedef struct {
    uint32_t magic;
    uint16_t version, flags;
    uint64_t request_id;
    uint8_t lease[16];
    uint64_t generation;
    uint64_t target_handle;
    uint32_t packet_bytes, reserved;
    uint64_t reserved2;
} RDGPUDrawBatchRequest;
_Static_assert(sizeof(RDGPUDrawBatchRequest)==64,"draw batch header size");
#endif
