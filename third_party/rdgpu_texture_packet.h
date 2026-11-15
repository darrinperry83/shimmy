#pragma once
#import "rdgpu_texture_store.h"

/* Only binds texture identity for a v2 draw. The full draw validator still
 * checks shaders, vertex layout and target lease before any GPU command. */
typedef BOOL (*RDGPUTextureMap)(uint64_t guestHandle,uint64_t guestGeneration,
                                void *context,RDGPUTextureKey *hostKey);
id<MTLTexture> rdgpuTextureForDraw(RDGPUTextureStore *store,
                                  const void *packet,size_t bytes,pid_t peer,
                                  RDGPUTextureMap map,void *context,
                                  RDGPUTextureKey *outKey);
