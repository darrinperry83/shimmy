#ifndef RDGPU_EXPORT_TOKEN_H
#define RDGPU_EXPORT_TOKEN_H
#include <stdint.h>
/* Exact bytes copied to the guest DRI image for an opt-in Wayland buffer.
 * This is a lookup key and geometry, never a credential or IOSurface ID. */
typedef struct {
    uint8_t window_token[16];
    uint64_t sequence;
    uint32_t width,height,stride,format;
} RDGPUExportImage;
_Static_assert(sizeof(RDGPUExportImage)==40,"export image ABI");
#define RDGPU_EXPORT_FORMAT_ARGB8888 0u
#endif
