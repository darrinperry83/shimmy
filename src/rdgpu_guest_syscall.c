#include "rdgpu_guest_syscall.h"
#include "rdmetal_draw_wire.h"
#include <errno.h>
#include <stdio.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void rdgpu_guest_state_init(RDGPUGuestState *s,pid_t attestedPid,
                            RDGPUTransport transport,void *context){
    if(!s)return;
    memset(s,0,sizeof *s);
    pthread_mutex_init(&s->lock,NULL);
    s->ownerPid=attestedPid;
    s->transport=transport;s->context=context;
    s->ready=attestedPid>0&&transport!=NULL;
}
void rdgpu_guest_set_export_authorizer(RDGPUGuestState *s,
                                      RDGPUExportAuthorize authorize,
                                      void *context){
    if(!s||!s->ready)return;
    pthread_mutex_lock(&s->lock);
    s->exportAuthorize=authorize;
    s->exportContext=context;
    pthread_mutex_unlock(&s->lock);
}
void rdgpu_guest_set_export_transport(RDGPUGuestState *s,
                                     RDGPUExportIssue issue,
                                     RDGPUExportAbort abortExport,
                                     RDGPUExportCopyout copyout,
                                     void *context){
    if(!s||!s->ready)return;
    pthread_mutex_lock(&s->lock);
    s->exportIssue=issue;s->exportAbort=abortExport;
    s->exportCopyout=copyout;s->exportTransportContext=context;
    pthread_mutex_unlock(&s->lock);
}
void rdgpu_guest_set_draw_transport(RDGPUGuestState *s,
                                   RDGPUDrawBatchTransport transport,
                                   RDGPUDrawPollTransport poll,
                                   void *context){
    if(!s||!s->ready)return;
    pthread_mutex_lock(&s->lock);
    s->drawTransport=transport;s->drawPollTransport=poll;s->drawContext=context;
    pthread_mutex_unlock(&s->lock);
}
void rdgpu_guest_set_fd_export_transport(RDGPUGuestState *s,
                                        RDGPUFDExportIssue issue,void *context){
    if(!s||!s->ready)return;
    pthread_mutex_lock(&s->lock);
    s->fdExportIssue=issue;s->fdExportContext=context;
    pthread_mutex_unlock(&s->lock);
}
void rdgpu_guest_set_fd_import_transport(RDGPUGuestState *s,
                                         RDGPUFDImportResolve resolve,void *context){
    if(!s||!s->ready)return;
    pthread_mutex_lock(&s->lock);
    s->fdImportResolve=resolve;s->fdImportContext=context;
    pthread_mutex_unlock(&s->lock);
}
void rdgpu_guest_set_image_stride_transport(RDGPUGuestState *s,
                                            RDGPUImageStrideQuery query,void *context){
    if(!s||!s->ready)return;
    pthread_mutex_lock(&s->lock);
    s->imageStrideQuery=query;s->imageStrideContext=context;
    pthread_mutex_unlock(&s->lock);
}
static RDGPUGuestSlot *slot(RDGPUGuestState *s,uint64_t handle){
    if(!handle)return NULL;
    for(unsigned i=0;i<RDGPU_MAX_IMAGES_PER_PEER;i++)
        if(s->slots[i].handle==handle)return &s->slots[i];
    return NULL;
}
static int handle_taken(RDGPUGuestState *s,uint64_t handle){
    if(!handle)return 1;
    for(unsigned i=0;i<RDGPU_MAX_IMAGES_PER_PEER;i++)
        if(s->slots[i].handle==handle||s->slots[i].exportHandle==handle)return 1;
    return 0;
}
static long mapped(uint16_t status){
    switch(status){
      case RDGPU_OK:return 0;
      case RDGPU_PENDING:return -EAGAIN;
      case RDGPU_STALE:return -ESTALE;
      case RDGPU_LIMIT:return -ENOMEM;
      case RDGPU_BAD_STATE:return -EBUSY;
      case RDGPU_BAD_MESSAGE:return -EINVAL;
      default:return -EPERM;
    }
}
static int valid_reply(const RDGPURequest *q,const RDGPUReply *r){
    return r->magic==RDGPU_WIRE_MAGIC&&r->version==RDGPU_WIRE_VERSION&&
           r->bytes==RDGPU_WIRE_BYTES&&r->request_id==q->request_id&&
           r->flags==0&&r->reserved==0;
}
long rdgpu_guest_syscall(RDGPUGuestState *s,uint64_t op,uint64_t a1,
                          uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5){
    if(!s||!s->ready||s->disabled||getpid()!=s->ownerPid)return -EPERM;
    if(op==RDGPU_GUEST_IMAGE_STRIDE){
        if(!a1||!a2||!a3||a4||a5)return -EINVAL;
        pthread_mutex_lock(&s->lock);
        RDGPUGuestSlot *entry=slot(s,a1);
        if(!entry||entry->width!=a2||entry->height!=a3){
            fprintf(stderr,"STRIDE_FAIL entry=%p a1=%llu w=%llu h=%llu\n",(void*)entry,(unsigned long long)a1,(unsigned long long)a2,(unsigned long long)a3);
            pthread_mutex_unlock(&s->lock);return -ESTALE;
        }
        if(entry->drawFence||!s->imageStrideQuery){
            fprintf(stderr,"STRIDE_FAIL complete=%d fence=%llu cb=%p\n",entry->producerComplete,(unsigned long long)entry->drawFence,(void*)s->imageStrideQuery);
            pthread_mutex_unlock(&s->lock);return -EPERM;
        }
        long stride=s->imageStrideQuery(s->imageStrideContext,s->ownerPid,
            entry->lease,entry->generation,entry->width,entry->height);
        if(stride<(long)entry->width*4||stride>2048L*4||stride%4){
            pthread_mutex_unlock(&s->lock);return -EIO;
        }
        pthread_mutex_unlock(&s->lock);return stride;
    }
    if(op==RDGPU_GUEST_FD_EXPORT){
        if(!a1||!a2||!a3||a4||a5)return -EINVAL;
        pthread_mutex_lock(&s->lock);
        RDGPUGuestSlot *entry=slot(s,a1);
        if(!entry||entry->width!=a2||entry->height!=a3){
            pthread_mutex_unlock(&s->lock);return -ESTALE;
        }
        if(entry->exported||entry->drawFence||!s->fdExportIssue||
           !s->imageStrideQuery){pthread_mutex_unlock(&s->lock);return -EPERM;}
        long stride=s->imageStrideQuery(s->imageStrideContext,s->ownerPid,
            entry->lease,entry->generation,entry->width,entry->height);
        if(stride<(long)entry->width*4||stride>2048L*4||stride%4){
            pthread_mutex_unlock(&s->lock);return -EIO;
        }
        long fd=s->fdExportIssue(s->fdExportContext,s->ownerPid,
             entry->lease,entry->generation,a1,entry->lastCompletedFence,
             entry->width,entry->height,(uint32_t)stride);
        if(fd>=0)entry->exported=1;
        pthread_mutex_unlock(&s->lock);
        return fd;
    }
    if(op==RDGPU_GUEST_FD_IMPORT){
        if(!a1||a2||a3||a4||a5)return -EINVAL;
        if(!s->fdImportResolve)return -EPERM;
        pthread_mutex_lock(&s->lock);
        uint8_t lease[16];
        int rc=s->fdImportResolve(s->fdImportContext,s->ownerPid,(int)a1,lease);
        if(rc<0){pthread_mutex_unlock(&s->lock);return rc;}
        /* Map the resolved broker lease back to the original opaque handle so
         * the driver can reuse the exact IOSurface-backed resource it exported,
         * instead of allocating a fresh one. */
        RDGPUGuestSlot *import_entry=NULL;
        for(unsigned i=0;i<RDGPU_MAX_IMAGES_PER_PEER;i++)
            if(s->slots[i].handle&&!memcmp(s->slots[i].lease,lease,16)){
                import_entry=&s->slots[i];break;
            }
        if(!import_entry){pthread_mutex_unlock(&s->lock);return -ESTALE;}
        long handle=(long)import_entry->handle;
        pthread_mutex_unlock(&s->lock);
        return handle;
    }
    if(op==RDGPU_GUEST_DRAW_POLL){
        if(!a1||!a2||a3||a4||a5)return -EINVAL;
        pthread_mutex_lock(&s->lock);
        RDGPUGuestSlot *entry=slot(s,a1);
        if(!entry||entry->drawFence!=a2||!s->drawPollTransport){
            pthread_mutex_unlock(&s->lock);return -ESTALE;
        }
        int result=s->drawPollTransport(s->drawContext,a2);
        if(result>0||result<-(int)4095){s->disabled=1;result=-EIO;}
        if(result==0){entry->lastCompletedFence=a2;entry->drawFence=0;entry->producerComplete=1;}
        else if(result!=-EAGAIN){entry->drawFence=0;entry->producerComplete=0;}
        pthread_mutex_unlock(&s->lock);
        return result;
    }
    if(op==RDGPU_GUEST_DRAW_BATCH){
        if(!a1||!a2||a3<sizeof(struct rdmetal_draw_header)||
           a3>RDGPU_DRAW_BATCH_MAX_BYTES||a4||a5)return -EINVAL;
        pthread_mutex_lock(&s->lock);
        RDGPUGuestSlot *entry=slot(s,a1);
        if(!entry){pthread_mutex_unlock(&s->lock);return -ESTALE;}
        if(entry->exported||entry->drawFence){pthread_mutex_unlock(&s->lock);return -EBUSY;}
        if(!s->drawTransport||!s->drawPollTransport){pthread_mutex_unlock(&s->lock);return -EPERM;}
        void *snapshot=malloc((size_t)a3);
        if(!snapshot){pthread_mutex_unlock(&s->lock);return -ENOMEM;}
        mach_vm_size_t copied=0;
        kern_return_t status=mach_vm_read_overwrite(mach_task_self(),
            (mach_vm_address_t)a2,(mach_vm_size_t)a3,
            (mach_vm_address_t)snapshot,&copied);
        if(status!=KERN_SUCCESS||copied!=a3){
            free(snapshot);pthread_mutex_unlock(&s->lock);return -EFAULT;
        }
        struct rdmetal_draw_header draw;
        memcpy(&draw,snapshot,sizeof draw);
        if(draw.magic!=RDMETAL_DRAW_MAGIC||draw.version!=RDMETAL_DRAW_VERSION||
           draw.total_bytes!=a3||draw.target_handle!=a1){
            free(snapshot);pthread_mutex_unlock(&s->lock);return -EINVAL;
        }
        RDGPUDrawBatchRequest batch={.magic=RDGPU_DRAW_BATCH_MAGIC,
            .version=RDGPU_DRAW_BATCH_VERSION,.request_id=++s->nextRequest,
            .generation=entry->generation,.target_handle=a1,
            .packet_bytes=(uint32_t)a3};
        memcpy(batch.lease,entry->lease,16);
        long result=s->drawTransport(s->drawContext,&batch,snapshot,(size_t)a3);
        free(snapshot);
        if(result>0){entry->drawFence=(uint64_t)result;entry->producerComplete=0;
                     entry->lastCompletedFence=0;}
        else if(result==0||result<-(long)4095){s->disabled=1;result=-EIO;}
        else entry->producerComplete=0;
        pthread_mutex_unlock(&s->lock);
        return result;
    }
    if(a4||a5||op<RDGPU_CREATE_BGRA8||op>RDGPU_PREPARE_EXPORT)return -EINVAL;
    if(op==RDGPU_PREPARE_EXPORT){
        if(!a2||a3!=sizeof(RDGPUExportImage))return -EINVAL;
    }else if(a3)return -EINVAL;
    pthread_mutex_lock(&s->lock);
    RDGPURequest q={.magic=RDGPU_WIRE_MAGIC,.version=RDGPU_WIRE_VERSION,
                    .op=(uint16_t)op,.bytes=RDGPU_WIRE_BYTES,
                    .request_id=++s->nextRequest};
    RDGPUGuestSlot *entry=NULL;uint64_t newHandle=0;
    if(op==RDGPU_CREATE_BGRA8){
        if(!a1||!a2||a1>RDGPU_MAX_IMAGE_EDGE||a2>RDGPU_MAX_IMAGE_EDGE||
           a1*a2*4>RDGPU_MAX_IMAGE_BYTES){pthread_mutex_unlock(&s->lock);return -EINVAL;}
        unsigned freeSlot=RDGPU_MAX_IMAGES_PER_PEER;
        for(unsigned i=0;i<RDGPU_MAX_IMAGES_PER_PEER;i++)
            if(!s->slots[i].handle){freeSlot=i;break;}
        if(freeSlot==RDGPU_MAX_IMAGES_PER_PEER){pthread_mutex_unlock(&s->lock);return -ENOMEM;}
        for(unsigned attempt=0;attempt<8&&!newHandle;attempt++){
            arc4random_buf(&newHandle,sizeof newHandle);
            newHandle &= INT64_MAX; // Linux syscall success must be nonnegative.
            if(handle_taken(s,newHandle))newHandle=0;
        }
        if(!newHandle){pthread_mutex_unlock(&s->lock);return -ENOMEM;}
        entry=&s->slots[freeSlot];q.arg0=a1|(a2<<32);
    }else{
        entry=slot(s,a1);
        if(!entry){pthread_mutex_unlock(&s->lock);return -ESTALE;}
        memcpy(q.lease,entry->lease,16);q.arg1=entry->generation;
        q.arg0=op==RDGPU_RELEASE?0:a2;
        if(op==RDGPU_RELEASE&&a2){pthread_mutex_unlock(&s->lock);return -EINVAL;}
        if(op==RDGPU_WAIT&&!a2){pthread_mutex_unlock(&s->lock);return -EINVAL;}
        if(op==RDGPU_CLEAR&&entry->exported){
            pthread_mutex_unlock(&s->lock);return -EBUSY;
        }
        if(entry->drawFence&&(op==RDGPU_CLEAR||op==RDGPU_RELEASE||
                              op==RDGPU_PREPARE_EXPORT)){
            pthread_mutex_unlock(&s->lock);return -EBUSY;
        }
        if(op==RDGPU_PREPARE_EXPORT){
            if(!entry->producerComplete||entry->exported){
                pthread_mutex_unlock(&s->lock);return -EBUSY;
            }
            if(!s->exportAuthorize||!s->exportIssue||!s->exportAbort||
               !s->exportCopyout||
               !s->exportAuthorize(s->exportContext,s->ownerPid,
                                   entry->lease,entry->generation)){
                pthread_mutex_unlock(&s->lock);return -EPERM;
            }
            for(unsigned attempt=0;attempt<8&&!newHandle;attempt++){
                arc4random_buf(&newHandle,sizeof newHandle);
                newHandle &= INT64_MAX;
                if(handle_taken(s,newHandle))newHandle=0;
            }
            if(!newHandle){pthread_mutex_unlock(&s->lock);return -ENOMEM;}
            q.arg0=newHandle;
            RDGPUReply exportReply={0};RDGPUExportImage image={0};
            int issued=s->exportIssue(s->exportTransportContext,s->ownerPid,
                                      &q,&exportReply,&image);
            uint8_t tokenAny=0;
            for(unsigned i=0;i<16;i++)tokenAny|=image.window_token[i];
            if(issued||!valid_reply(&q,&exportReply)||
               memcmp(exportReply.lease,entry->lease,16)||
               exportReply.status!=RDGPU_OK||!exportReply.value0||
               exportReply.value1||!tokenAny||!image.sequence||
               image.width!=entry->width||image.height!=entry->height||
               image.width>RDGPU_MAX_IMAGE_EDGE||
               image.stride<(uint64_t)image.width*4||
               image.stride>RDGPU_MAX_IMAGE_EDGE*4||
               image.format!=RDGPU_EXPORT_FORMAT_ARGB8888){
                if(tokenAny)s->exportAbort(s->exportTransportContext,s->ownerPid,
                                            image.window_token,image.sequence);
                s->disabled=1;pthread_mutex_unlock(&s->lock);return -EIO;
            }
            if(s->exportCopyout(s->exportTransportContext,a2,&image,sizeof image)){
                s->exportAbort(s->exportTransportContext,s->ownerPid,
                               image.window_token,image.sequence);
                pthread_mutex_unlock(&s->lock);return -EFAULT;
            }
            entry->exportHandle=newHandle;entry->brokerReceipt=exportReply.value0;
            entry->exported=1;
            pthread_mutex_unlock(&s->lock);return (long)newHandle;
        }
    }
    RDGPUReply r=s->transport(s->context,&q);
    if(!valid_reply(&q,&r) ||
       (op!=RDGPU_CREATE_BGRA8 && memcmp(r.lease,entry->lease,16))){
        s->disabled=1;pthread_mutex_unlock(&s->lock);return -EIO;
    }
    long result=mapped(r.status);
    if(result==0&&op==RDGPU_CREATE_BGRA8){
        uint8_t any=0;for(unsigned i=0;i<16;i++)any|=r.lease[i];
        if(!any||!r.value1){s->disabled=1;result=-EIO;}
        else {entry->handle=newHandle;entry->generation=r.value1;
              entry->width=(uint32_t)a1;entry->height=(uint32_t)a2;
              memcpy(entry->lease,r.lease,16);result=(long)newHandle;}
    }else if(result==0){
        if(op==RDGPU_CLEAR){
            if(r.value0&&r.value0<=INT64_MAX)result=(long)r.value0;
            else{s->disabled=1;result=-EIO;}
        }
        else if(op==RDGPU_WAIT){entry->producerComplete=1;entry->lastCompletedFence=a2;}
        else if(op==RDGPU_PREPARE_EXPORT){
            if(!r.value0||r.value1){s->disabled=1;result=-EIO;}
            else {entry->exportHandle=newHandle;entry->brokerReceipt=r.value0;
                  entry->exported=1;result=(long)newHandle;}
        }
        else if(op==RDGPU_RELEASE){memset(entry,0,sizeof *entry);}
    }
    if(result>0&&op==RDGPU_CLEAR){entry->producerComplete=0;entry->lastCompletedFence=0;}
    pthread_mutex_unlock(&s->lock);
    return result;
}
