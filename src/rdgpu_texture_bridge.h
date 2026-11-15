#ifndef RDGPU_TEXTURE_BRIDGE_H
#define RDGPU_TEXTURE_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* C-callable bridge over the ObjC RDGPUTextureStore so the C mock gate can
 * upload/retire host MTLTextures and hand them to the draw host. */
typedef struct RDGPUTextureBridge RDGPUTextureBridge;

RDGPUTextureBridge *rdgpu_texture_bridge_create(pid_t peer);
void rdgpu_texture_bridge_destroy(RDGPUTextureBridge *bridge);
/* Returns a positive opaque texture handle or a negative errno. *generation is
 * set on success. The pixel bytes are copied into an MTLTexture synchronously. */
long rdgpu_texture_bridge_upload(RDGPUTextureBridge *bridge,
                                 uint32_t width, uint32_t height,
                                 const void *pixels, size_t bytes,
                                 uint64_t *generation);
int rdgpu_texture_bridge_release(RDGPUTextureBridge *bridge,
                                 uint64_t handle, uint64_t generation);
/* Borrow the MTLTexture for a draw (increments in-flight). Returns NULL on
 * failure. Call rdgpu_texture_bridge_complete when the draw is done. */
void *rdgpu_texture_bridge_acquire(RDGPUTextureBridge *bridge,
                                   uint64_t handle, uint64_t generation);
void rdgpu_texture_bridge_complete(RDGPUTextureBridge *bridge,
                                   uint64_t handle, uint64_t generation);
#endif
