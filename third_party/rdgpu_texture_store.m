#import "rdgpu_texture_store.h"
#import <Security/SecRandom.h>

enum { RDGPU_TEX_SLOTS = 8, RDGPU_TEX_MAX_EDGE = 4096,
       RDGPU_TEX_MAX_PIXELS = 1024 * 1024, RDGPU_TEX_MAX_BYTES = 16 * 1024 * 1024 };

@interface RDGPUTextureSlot : NSObject
@property(nonatomic,strong) id<MTLTexture> texture;
@property(nonatomic) uint64_t handle, generation;
@property(nonatomic) size_t bytes;
@property(nonatomic) unsigned inFlight;
@property(nonatomic) BOOL retired;
@end
@implementation RDGPUTextureSlot @end

@interface RDGPUTextureStore ()
@property(nonatomic,strong) id<MTLDevice> device;
@property(nonatomic) pid_t peer;
@property(nonatomic) RDGPUTextureVerifyPeer verify;
@property(nonatomic) void *verifyContext;
@property(nonatomic,strong) NSMutableArray<RDGPUTextureSlot *> *slots;
@property(nonatomic) uint64_t lastRequest;
@property(nonatomic) size_t activeBytes;
@property(nonatomic) BOOL disconnected;
@end

@implementation RDGPUTextureStore
- (instancetype)initWithDevice:(id<MTLDevice>)device actualPeer:(pid_t)peer
                       verify:(RDGPUTextureVerifyPeer)verify context:(void *)context {
    if(!device||peer<=0||!verify||!verify(peer,context))return nil;
    if((self=[super init])){
        _device=device;_peer=peer;_verify=verify;_verifyContext=context;
        _slots=[NSMutableArray new];
    }
    return self;
}
- (RDGPUTextureSlot *)find:(RDGPUTextureKey)key {
    if(!key.handle||!key.generation)return nil;
    for(RDGPUTextureSlot *slot in self.slots)
        if(slot.texture&&slot.handle==key.handle&&slot.generation==key.generation)
            return slot;
    return nil;
}
- (BOOL)allowed:(pid_t)peer {
    return !self.disconnected&&peer==self.peer&&self.verify(peer,self.verifyContext);
}
- (BOOL)upload:(const void *)pixels bytes:(size_t)bytes
         width:(uint32_t)width height:(uint32_t)height format:(uint32_t)format
     requestID:(uint64_t)requestID actualPeer:(pid_t)peer
           key:(RDGPUTextureKey *)key {
    @synchronized(self) {
        if(!key||!pixels||![self allowed:peer]||!requestID||
           requestID<=self.lastRequest)return NO;
        self.lastRequest=requestID; // Invalid requests cannot be replayed.
        uint64_t count=(uint64_t)width*height,needed=count*4;
        if(!width||!height||width>RDGPU_TEX_MAX_EDGE||height>RDGPU_TEX_MAX_EDGE||
           count>RDGPU_TEX_MAX_PIXELS||format!=RDGPU_TEXTURE_RGBA8||
           needed!=bytes||needed>RDGPU_TEX_MAX_BYTES-self.activeBytes)return NO;
        RDGPUTextureSlot *slot=nil;
        for(RDGPUTextureSlot *item in self.slots)
            if(!item.texture){slot=item;break;}
        if(!slot){
            if(self.slots.count>=RDGPU_TEX_SLOTS)return NO;
            slot=[RDGPUTextureSlot new];[self.slots addObject:slot];
        }
        NSData *snapshot=[NSData dataWithBytes:pixels length:bytes];
        MTLTextureDescriptor *desc=[MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:width height:height mipmapped:NO];
        desc.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> texture=[self.device newTextureWithDescriptor:desc];
        if(!snapshot||!texture)return NO;
        [texture replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0
                    withBytes:snapshot.bytes bytesPerRow:(NSUInteger)width*4];
        uint64_t handle=0;
        BOOL unique=NO;
        for(unsigned attempt=0;attempt<4&&!unique;attempt++){
            if(SecRandomCopyBytes(kSecRandomDefault,sizeof handle,&handle)!=errSecSuccess)
                return NO;
            handle&=INT64_MAX; // the guest syscall return value must be nonnegative
            unique=handle!=0;
            for(RDGPUTextureSlot *item in self.slots)
                if(item.texture&&item.handle==handle)unique=NO;
        }
        if(!unique)return NO;
        slot.texture=texture;slot.handle=handle;slot.bytes=bytes;
        if(++slot.generation==0)++slot.generation;
        slot.inFlight=0;slot.retired=NO;self.activeBytes+=bytes;
        *key=(RDGPUTextureKey){handle,slot.generation};
        return YES;
    }
}
- (id<MTLTexture>)acquire:(RDGPUTextureKey)key actualPeer:(pid_t)peer {
    @synchronized(self) {
        if(![self allowed:peer])return nil;
        RDGPUTextureSlot *slot=[self find:key];
        if(!slot||slot.retired||slot.inFlight==UINT32_MAX)return nil;
        slot.inFlight++;
        return slot.texture;
    }
}
- (BOOL)complete:(RDGPUTextureKey)key actualPeer:(pid_t)peer {
    @synchronized(self) {
        if(peer!=self.peer)return NO; // Completion may follow disconnect.
        RDGPUTextureSlot *slot=[self find:key];
        if(!slot||!slot.inFlight)return NO;
        slot.inFlight--;
        if(!slot.inFlight&&slot.retired){
            self.activeBytes-=slot.bytes;slot.bytes=0;slot.texture=nil;slot.handle=0;
        }
        return YES;
    }
}
- (BOOL)retire:(RDGPUTextureKey)key actualPeer:(pid_t)peer {
    @synchronized(self) {
        if(![self allowed:peer])return NO;
        RDGPUTextureSlot *slot=[self find:key];
        if(!slot||slot.retired)return NO;
        slot.retired=YES;
        if(!slot.inFlight){
            self.activeBytes-=slot.bytes;slot.bytes=0;slot.texture=nil;slot.handle=0;
        }
        return YES;
    }
}
- (void)disconnect {
    @synchronized(self) {
        if(self.disconnected)return;
        self.disconnected=YES;
        for(RDGPUTextureSlot *slot in self.slots)if(slot.texture){
            slot.retired=YES;
            if(!slot.inFlight){
                self.activeBytes-=slot.bytes;slot.bytes=0;slot.texture=nil;slot.handle=0;
            }
        }
    }
}
@end
