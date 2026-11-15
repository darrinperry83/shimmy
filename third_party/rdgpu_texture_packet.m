#import "rdgpu_texture_packet.h"
#include "rdmetal_draw_wire.h"
#include <string.h>

id<MTLTexture> rdgpuTextureForDraw(RDGPUTextureStore *store,
                                  const void *packet,size_t bytes,pid_t peer,
                                  RDGPUTextureMap map,void *context,
                                  RDGPUTextureKey *outKey){
    if(!store||!packet||!map||!outKey||
       bytes<sizeof(struct rdmetal_draw_header_v2))
        return nil;
    struct rdmetal_draw_header_v2 h;
    memcpy(&h,packet,sizeof h);
    if(h.base.magic!=RDMETAL_DRAW_MAGIC||
       h.base.version!=RDMETAL_DRAW_VERSION_TEXTURE||
       h.base.total_bytes!=bytes||h.base.flags||h.reserved||h.sampler_flags||
       !h.texture_handle||!h.texture_generation||
       h.base.vertex_shader_words<5||h.base.fragment_shader_words<5||
       h.base.vertex_shader_words>RDMETAL_DRAW_MAX_SHADER_WORDS||
       h.base.fragment_shader_words>RDMETAL_DRAW_MAX_SHADER_WORDS||
       h.base.vertex_bytes>RDMETAL_DRAW_MAX_VERTEX_BYTES)return nil;
    uint64_t payload=(uint64_t)h.base.vertex_shader_words*4+
        (uint64_t)h.base.fragment_shader_words*4+h.base.vertex_bytes;
    if(payload+sizeof h!=bytes)return nil;
    const uint8_t *data=(const uint8_t *)packet+sizeof h;
    uint32_t vsMagic=0,fsMagic=0;
    memcpy(&vsMagic,data,sizeof vsMagic);
    memcpy(&fsMagic,data+(size_t)h.base.vertex_shader_words*4,sizeof fsMagic);
    if(vsMagic!=0x07230203u||fsMagic!=0x07230203u)return nil;
    RDGPUTextureKey key={0};
    if(!map(h.texture_handle,h.texture_generation,context,&key)||
       !key.handle||!key.generation)return nil;
    id<MTLTexture> texture=[store acquire:key actualPeer:peer];
    if(texture)*outKey=key;
    return texture;
}
