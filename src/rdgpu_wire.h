#ifndef RDGPU_WIRE_H
#define RDGPU_WIRE_H
#include <stdint.h>

/* Fixed-size, little-endian protocol on an authenticated local connection.
 * The connection's peer identity is checked by the host before CREATE;
 * possession of a numeric lease never authenticates a different peer. */
#define RDGPU_WIRE_MAGIC 0x55475052u /* "RPGU" little-endian */
#define RDGPU_WIRE_VERSION 1u
#define RDGPU_WIRE_BYTES 64u
#define RDGPU_MAX_IMAGE_EDGE 2048u
#define RDGPU_MAX_IMAGE_BYTES (16u*1024u*1024u)
#define RDGPU_MAX_IMAGES_PER_PEER 16u
#define RDGPU_MAX_BYTES_PER_PEER (128u*1024u*1024u)

enum RDGPUOp { RDGPU_CREATE_BGRA8=1, RDGPU_CLEAR=2, RDGPU_WAIT=3,
               RDGPU_RELEASE=4, RDGPU_PREPARE_EXPORT=5 };
enum RDGPUStatus { RDGPU_OK=0, RDGPU_BAD_MESSAGE=1, RDGPU_DENIED=2,
                   RDGPU_STALE=3, RDGPU_LIMIT=4, RDGPU_BAD_STATE=5,
                   RDGPU_GPU_FAILED=6, RDGPU_PENDING=7 };

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version, op;
    uint32_t bytes, flags;
    uint64_t request_id; /* strictly increasing within one connection */
    uint8_t lease[16];    /* all zero on CREATE; opaque on later operations */
    uint64_t arg0, arg1;
    uint64_t reserved;
} RDGPURequest;
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version, status;
    uint32_t bytes, flags;
    uint64_t request_id;
    uint8_t lease[16];    /* newly random on CREATE; echoed otherwise */
    uint64_t value0, value1;
    uint64_t reserved;
} RDGPUReply;

/* CREATE: arg0=width|(height<<32), arg1=0; reply value1=slot generation.
 * CLEAR: arg0 contains four little-endian UNORM16 channels R,G,B,A,
 * arg1=generation; reply value0=fence sequence.
 * WAIT: arg0=fence sequence, arg1=generation.
 * RELEASE: arg0=0, arg1=generation.
 * PREPARE_EXPORT: arg0=random local export handle, arg1=generation; reply
 * value0 is a nonzero broker receipt retained only by the shim. The broker
 * indexes the local handle by its attested guest owner; the handle alone is
 * never authority. Redemption also requires an authenticated compositor
 * peer and exact server-side Wayland client/buffer mapping.
 * This candidate does not implement the host broker side of that operation.
 * All flags/reserved fields must be zero. An image admits one unfinished clear.
 * WAIT is a nonblocking poll: PENDING until actual Metal command-buffer
 * completion, then OK. It is not display scanout.
 * RELEASE waits for pending work before destroying the IOSurface-backed image. */
_Static_assert(sizeof(RDGPURequest)==RDGPU_WIRE_BYTES,"request size");
_Static_assert(sizeof(RDGPUReply)==RDGPU_WIRE_BYTES,"reply size");
#endif
