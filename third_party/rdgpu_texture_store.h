#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct { uint64_t handle, generation; } RDGPUTextureKey;
typedef BOOL (*RDGPUTextureVerifyPeer)(pid_t actualPeer, void *context);
enum { RDGPU_TEXTURE_RGBA8 = 1 };

/* Host-only candidate. The peer PID comes from an authenticated connection;
 * a guest-supplied PID or opaque key by itself grants no texture access. */
@interface RDGPUTextureStore : NSObject
- (instancetype)initWithDevice:(id<MTLDevice>)device
                   actualPeer:(pid_t)peer
                       verify:(RDGPUTextureVerifyPeer)verify
                      context:(void *)context;
- (BOOL)upload:(const void *)pixels bytes:(size_t)bytes
         width:(uint32_t)width height:(uint32_t)height format:(uint32_t)format
     requestID:(uint64_t)requestID actualPeer:(pid_t)peer
           key:(RDGPUTextureKey *)key;
- (id<MTLTexture>)acquire:(RDGPUTextureKey)key actualPeer:(pid_t)peer;
- (BOOL)complete:(RDGPUTextureKey)key actualPeer:(pid_t)peer;
- (BOOL)retire:(RDGPUTextureKey)key actualPeer:(pid_t)peer;
- (void)disconnect;
@end
