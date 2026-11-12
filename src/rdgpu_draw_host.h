#ifndef RDGPU_DRAW_HOST_H
#define RDGPU_DRAW_HOST_H
#include <IOSurface/IOSurface.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host-only resolver. It must authenticate the caller and return a retained
 * surface for the opaque handle, or NULL. The draw adapter releases it. */
typedef IOSurfaceRef (*RDGPUDrawResolve)(uint64_t handle, void *context);
enum RDGPUDrawResult {
    RDGPU_DRAW_OK=0, RDGPU_DRAW_BAD_PACKET=1, RDGPU_DRAW_UNSUPPORTED=2,
    RDGPU_DRAW_DENIED=3, RDGPU_DRAW_SHADER_FAILED=4, RDGPU_DRAW_GPU_FAILED=5
};
int rdgpu_draw_execute(const void *packet,size_t bytes,
                       RDGPUDrawResolve resolve,void *context);

#ifdef __cplusplus
}
#endif
#endif
