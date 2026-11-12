#pragma once
#include <stdint.h>
#define RDGPU_FD_ISSUE_MAGIC 0x52444644u
#define RDGPU_FD_ISSUE_VERSION 1u
#define RDGPU_FD_ISSUE_ARGB8888 0x34325241u
/* Host shim constructs this only after exact op1 slot and completed fence.
 * No guest-supplied token, FD, pointer, or IOSurface ID is accepted. */
typedef struct {
    uint32_t magic;
    uint16_t version,bytes;
    uint8_t lease[16];
    uint64_t generation;
    uint64_t completedFence;
    uint32_t width,height,stride,fourcc;
    uint64_t modifier;
} RDGPUFDIssueWire;
_Static_assert(sizeof(RDGPUFDIssueWire)==64,"fd issue wire ABI");
