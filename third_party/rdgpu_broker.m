#include "rdgpu_broker.h"
#include <Security/SecRandom.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    RDMetalImage *image;
    uint8_t lease[16];
    uint64_t generation, fence, bytes, exportReceipt;
    unsigned exportRefs;
    bool completed;
} ImageSlot;
struct RDGPUSession {
    RDMetalCore *core;
    pthread_mutex_t lock;
    uint64_t lastRequest, bytes;
    pid_t actualPeer;
    RDGPUAuthorizeTestExport authorizeExport;
    RDGPURevokeTestExport revokeExport;
    void *exportContext;
    ImageSlot images[RDGPU_MAX_IMAGES_PER_PEER];
};
static int zero(const void *data,size_t count) {
    const uint8_t *p=data;uint8_t any=0;
    for(size_t i=0;i<count;i++)any|=p[i];
    return any==0;
}
static ImageSlot *find(RDGPUSession *s,const uint8_t lease[16],uint64_t generation){
    if(!generation||zero(lease,16))return NULL;
    for(unsigned i=0;i<RDGPU_MAX_IMAGES_PER_PEER;i++){
        ImageSlot *slot=&s->images[i];uint8_t diff=0;
        for(unsigned j=0;j<16;j++)diff|=slot->lease[j]^lease[j];
        if(slot->image&&slot->generation==generation&&!diff)return slot;
    }
    return NULL;
}
static RDGPUReply reply(const RDGPURequest *q,uint16_t status){
    RDGPUReply r={.magic=RDGPU_WIRE_MAGIC,.version=RDGPU_WIRE_VERSION,
                  .status=status,.bytes=RDGPU_WIRE_BYTES,
                  .request_id=q?q->request_id:0};
    if(q)memcpy(r.lease,q->lease,16);
    return r;
}
RDGPUSession *rdgpu_test_session_create(pid_t actualPeer,
                                        RDGPUVerifyTestPeer verify,void *context){
    // A null verifier fails closed. The caller obtains actualPeer from the OS,
    // not a wire field; this entry is deliberately named as a test API.
    if(actualPeer<=0||!verify||!verify(actualPeer,context))return NULL;
    RDGPUSession *s=calloc(1,sizeof *s);if(!s)return NULL;
    if(pthread_mutex_init(&s->lock,NULL)){free(s);return NULL;}
    s->actualPeer=actualPeer;
    s->core=rd_metal_core_create();
    if(!s->core){pthread_mutex_destroy(&s->lock);free(s);return NULL;}
    return s;
}
void rdgpu_test_set_export_callbacks(RDGPUSession *s,
                                     RDGPUAuthorizeTestExport authorize,
                                     RDGPURevokeTestExport revoke,void *context){
    if(!s)return;
    pthread_mutex_lock(&s->lock);
    // The two callbacks must be installed as a pair; no export may be live.
    bool busy=false;
    for(unsigned i=0;i<RDGPU_MAX_IMAGES_PER_PEER;i++)
        busy|=s->images[i].exportReceipt!=0;
    if(!busy){s->authorizeExport=authorize&&revoke?authorize:NULL;
              s->revokeExport=authorize&&revoke?revoke:NULL;
              s->exportContext=authorize&&revoke?context:NULL;}
    pthread_mutex_unlock(&s->lock);
}
bool rdgpu_test_return_export(RDGPUSession *s,const uint8_t lease[16],
                              uint64_t generation,uint64_t receipt){
    if(!s||!receipt)return false;
    pthread_mutex_lock(&s->lock);
    ImageSlot *slot=find(s,lease,generation);
    bool valid=slot&&slot->exportReceipt==receipt&&slot->exportRefs==1;
    if(valid){slot->exportReceipt=0;slot->exportRefs=0;}
    pthread_mutex_unlock(&s->lock);
    return valid;
}
RDGPUReply rdgpu_dispatch(RDGPUSession *s,const RDGPURequest *q){
    if(!s)return reply(q,RDGPU_DENIED);
    if(!q||q->magic!=RDGPU_WIRE_MAGIC||q->version!=RDGPU_WIRE_VERSION||
       q->bytes!=RDGPU_WIRE_BYTES||q->flags||q->reserved||
       q->op<RDGPU_CREATE_BGRA8||q->op>RDGPU_PREPARE_EXPORT||!q->request_id)
        return reply(q,RDGPU_BAD_MESSAGE);
    pthread_mutex_lock(&s->lock);
    RDGPUReply r=reply(q,RDGPU_STALE);
    if(q->request_id<=s->lastRequest)goto done;
    s->lastRequest=q->request_id;
    r.status=RDGPU_BAD_MESSAGE;
    if(q->op==RDGPU_CREATE_BGRA8){
        if(!zero(q->lease,16)||q->arg1)goto done;
        uint32_t w=(uint32_t)q->arg0,h=(uint32_t)(q->arg0>>32);
        uint64_t bytes=(uint64_t)w*h*4;
        if(!w||!h||w>RDGPU_MAX_IMAGE_EDGE||h>RDGPU_MAX_IMAGE_EDGE||
           bytes>RDGPU_MAX_IMAGE_BYTES){r.status=RDGPU_LIMIT;goto done;}
        if(bytes>RDGPU_MAX_BYTES_PER_PEER-s->bytes){r.status=RDGPU_LIMIT;goto done;}
        ImageSlot *slot=NULL;
        for(unsigned i=0;i<RDGPU_MAX_IMAGES_PER_PEER;i++)if(!s->images[i].image){
            slot=&s->images[i];break;}
        if(!slot){r.status=RDGPU_LIMIT;goto done;}
        RDMetalImage *image=rd_metal_image_create(s->core,w,h);
        if(!image){r.status=RDGPU_GPU_FAILED;goto done;}
        uint8_t token[16];
        if(SecRandomCopyBytes(kSecRandomDefault,16,token)!=errSecSuccess||zero(token,16)){
            rd_metal_image_destroy(image);r.status=RDGPU_GPU_FAILED;goto done;}
        slot->image=image;slot->bytes=bytes;slot->fence=0;
        slot->exportRefs=0;slot->exportReceipt=0;slot->completed=false;
        if(++slot->generation==0)++slot->generation;
        memcpy(slot->lease,token,16);s->bytes+=bytes;
        memcpy(r.lease,token,16);r.value1=slot->generation;
        r.status=RDGPU_OK;goto done;
    }
    ImageSlot *slot=find(s,q->lease,q->arg1);
    if(!slot){r.status=RDGPU_STALE;goto done;}
    if(q->op==RDGPU_CLEAR){
        if(slot->fence||slot->exportRefs){r.status=RDGPU_BAD_STATE;goto done;}
        slot->completed=false;
        float rgba[4];for(unsigned i=0;i<4;i++)rgba[i]=((q->arg0>>(i*16))&0xffffu)/65535.f;
        slot->fence=rd_metal_image_clear(slot->image,rgba[0],rgba[1],rgba[2],rgba[3]);
        if(!slot->fence){r.status=RDGPU_GPU_FAILED;goto done;}
        r.value0=slot->fence;r.status=RDGPU_OK;goto done;
    }
    if(q->op==RDGPU_WAIT){
        if(!q->arg0||q->arg0!=slot->fence){r.status=RDGPU_BAD_STATE;goto done;}
        int state=rd_metal_image_poll(slot->image,q->arg0);
        if(state==0)r.status=RDGPU_PENDING;
        else {slot->fence=0;slot->completed=state>0;
              r.status=state>0?RDGPU_OK:RDGPU_GPU_FAILED;}
        goto done;
    }
    if(q->op==RDGPU_PREPARE_EXPORT){
        if(!q->arg0||q->arg0>INT64_MAX||slot->fence||!slot->completed||
           slot->exportRefs||!s->authorizeExport){r.status=RDGPU_DENIED;goto done;}
        IOSurfaceRef surface=rd_metal_image_surface(slot->image);
        uint64_t receipt=surface?s->authorizeExport(s->exportContext,s->actualPeer,
                                  surface,slot->lease,slot->generation):0;
        if(!receipt){r.status=RDGPU_DENIED;goto done;}
        slot->exportReceipt=receipt;slot->exportRefs=1;
        r.value0=receipt;r.value1=0;r.status=RDGPU_OK;goto done;
    }
    if(q->arg0||slot->fence||slot->exportRefs){r.status=RDGPU_BAD_STATE;goto done;}
    s->bytes-=slot->bytes;rd_metal_image_destroy(slot->image);
    slot->image=NULL;slot->bytes=0;slot->completed=false;slot->exportReceipt=0;
    memset(slot->lease,0,16);r.status=RDGPU_OK;
done:
    pthread_mutex_unlock(&s->lock);return r;
}
void rdgpu_session_disconnect(RDGPUSession *s){
    if(!s)return;
    uint64_t receipts[RDGPU_MAX_IMAGES_PER_PEER]={0};
    RDGPURevokeTestExport revoke=s->revokeExport;
    void *revokeContext=s->exportContext;
    pthread_mutex_lock(&s->lock);
    for(unsigned i=0;i<RDGPU_MAX_IMAGES_PER_PEER;i++){
        ImageSlot *slot=&s->images[i];
        receipts[i]=slot->exportReceipt;
        if(slot->image)rd_metal_image_destroy(slot->image);
        slot->image=NULL;memset(slot->lease,0,16);
        slot->exportReceipt=0;slot->exportRefs=0;
    }
    s->bytes=0;
    pthread_mutex_unlock(&s->lock);
    // The revoke callback may have its own lock; never call it under ours.
    if(revoke)for(unsigned i=0;i<RDGPU_MAX_IMAGES_PER_PEER;i++)
        if(receipts[i])revoke(revokeContext,receipts[i]);
    rd_metal_core_destroy(s->core);pthread_mutex_destroy(&s->lock);free(s);
}
IOSurfaceRef rdgpu_test_export_surface(RDGPUSession *s,const uint8_t lease[16],uint64_t generation){
    if(!s)return NULL;
    pthread_mutex_lock(&s->lock);
    ImageSlot *slot=find(s,lease,generation);
    IOSurfaceRef surface=slot&&slot->completed&&!slot->fence?
        rd_metal_image_surface(slot->image):NULL;
    if(surface&&!slot->exportReceipt){CFRetain(surface);slot->exportRefs++;}
    else surface=NULL;
    pthread_mutex_unlock(&s->lock);
    return surface;
}
bool rdgpu_test_return_surface(RDGPUSession *s,const uint8_t lease[16],
                               uint64_t generation,IOSurfaceRef surface){
    if(!s||!surface)return false;
    pthread_mutex_lock(&s->lock);
    ImageSlot *slot=find(s,lease,generation);
    bool valid=slot&&slot->exportRefs&&rd_metal_image_surface(slot->image)==surface;
    if(valid)slot->exportRefs--;
    pthread_mutex_unlock(&s->lock);
    if(valid)CFRelease(surface);
    return valid;
}
