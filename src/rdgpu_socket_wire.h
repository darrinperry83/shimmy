#ifndef RDGPU_SOCKET_WIRE_H
#define RDGPU_SOCKET_WIRE_H
#include "rdgpu_wire.h"
#include "rdgpu_fd_issue_wire.h"
#include <stdint.h>
#define RDGPU_SOCKET_MAGIC 0x53474452u
#define RDGPU_SOCKET_VERSION 1u
enum RDGPUSocketKind { RDGPU_SOCKET_REQUEST=1,
    RDGPU_SOCKET_STRIDE=2,RDGPU_SOCKET_FD_EXPORT=3 };
typedef struct {
    uint32_t magic,version,kind,bytes;
    uint64_t sequence;
    RDGPURequest request;
    RDGPUFDIssueWire image;
} RDGPUSocketCommand;
typedef struct {
    uint32_t magic,version,kind,bytes;
    uint64_t sequence;
    int32_t status;
    uint32_t stride;
    RDGPUReply reply;
} RDGPUSocketResponse;
#endif
