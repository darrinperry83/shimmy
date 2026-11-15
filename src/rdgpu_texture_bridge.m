#import "rdgpu_texture_bridge.h"
#import "rdgpu_texture_store.h"
#import <Metal/Metal.h>

struct RDGPUTextureBridge {
    void *store;              /* CFBridgingRetain'd RDGPUTextureStore */
    pid_t peer;
    uint64_t request;
};

static BOOL bridge_verify(pid_t pid, void *context) {
    return context && pid == *(pid_t *)context;
}

static RDGPUTextureStore *store_of(RDGPUTextureBridge *bridge) {
    return (__bridge RDGPUTextureStore *)bridge->store;
}

RDGPUTextureBridge *rdgpu_texture_bridge_create(pid_t peer) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
        return NULL;
    RDGPUTextureBridge *bridge = calloc(1, sizeof(*bridge));
    if (!bridge)
        return NULL;
    bridge->peer = peer;
    RDGPUTextureStore *store = [[RDGPUTextureStore alloc] initWithDevice:device
        actualPeer:peer verify:bridge_verify context:&bridge->peer];
    if (!store) {
        free(bridge);
        return NULL;
    }
    bridge->store = (void *)CFBridgingRetain(store);
    return bridge;
}

void rdgpu_texture_bridge_destroy(RDGPUTextureBridge *bridge) {
    if (!bridge)
        return;
    [store_of(bridge) disconnect];
    CFBridgingRelease(bridge->store);
    free(bridge);
}

long rdgpu_texture_bridge_upload(RDGPUTextureBridge *bridge,
                                 uint32_t width, uint32_t height,
                                 const void *pixels, size_t bytes,
                                 uint64_t *generation) {
    if (!bridge || !pixels || !generation)
        return -1;
    RDGPUTextureKey key = {0};
    uint64_t request = ++bridge->request;
    if (![store_of(bridge) upload:pixels bytes:bytes width:width height:height
        format:RDGPU_TEXTURE_RGBA8 requestID:request actualPeer:bridge->peer
        key:&key])
        return -5; /* -EIO */
    if (!key.handle || !key.generation)
        return -5;
    *generation = key.generation;
    return (long)key.handle;
}

int rdgpu_texture_bridge_release(RDGPUTextureBridge *bridge,
                                 uint64_t handle, uint64_t generation) {
    if (!bridge || !handle)
        return -1;
    return [store_of(bridge) retire:(RDGPUTextureKey){handle, generation}
        actualPeer:bridge->peer] ? 0 : -5;
}

void *rdgpu_texture_bridge_acquire(RDGPUTextureBridge *bridge,
                                   uint64_t handle, uint64_t generation) {
    if (!bridge || !handle)
        return NULL;
    id<MTLTexture> texture = [store_of(bridge)
        acquire:(RDGPUTextureKey){handle, generation} actualPeer:bridge->peer];
    return (__bridge void *)texture;
}

void rdgpu_texture_bridge_complete(RDGPUTextureBridge *bridge,
                                   uint64_t handle, uint64_t generation) {
    if (!bridge || !handle)
        return;
    [store_of(bridge) complete:(RDGPUTextureKey){handle, generation}
        actualPeer:bridge->peer];
}
