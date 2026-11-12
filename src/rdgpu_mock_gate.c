/* Isolated, opt-in test gate. It never connects to a Metal/XPC service. */
#include "rdgpu_mock_gate.h"
#include "rdgpu_guest_syscall.h"
#include "gmem.h"
#include "rdgpu_virtual_carrier.h"
#include "rdgpu_broker.h"
#include "rdgpu_draw_host.h"
#include "rdgpu_socket_wire.h"
#include "rdgpu_surface_handoff.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
static RDGPUGuestState state;
static RDGPUSession *metal_fixture;
static int socket_fixture=-1;
static uint64_t socket_sequence;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static uint8_t issued_token[16];
static uint8_t issued_token2[16];
static int have_token2;
static int init_failed;
static uint64_t seq;
static unsigned clear_count;
static unsigned image_count;
static uint64_t draw_fence_next,draw_fence_done;
static uint64_t broker_request_offset;
struct mock_image {
    uint8_t lease[16],token[16];
    uint32_t width,height;
    uint64_t sequence;
    int exported;
    RDCarrier *carrier;
    IOSurfaceRef retainedSurface;
    dev_t device;         /* carrier identity for the cross-process handoff */
    ino_t inode;
    uint64_t expectedClear;
    int hasExpectedClear;
};
static struct mock_image images[RDGPU_MAX_IMAGES_PER_PEER];
static unsigned image_limit(void){
    const char *p=getenv("RDMETAL_MOCK_FRAME_POOL");
    return p&&!strcmp(p,"2")?2:1;
}
static int parse_token(const char *hex,uint8_t output[16]){
    if(!hex||strlen(hex)!=32)return -1;
    unsigned nonzero=0;
    for(unsigned i=0;i<16;i++){
        unsigned a,b;char x=hex[i*2],y=hex[i*2+1];
        a=x>='0'&&x<='9'?x-'0':x>='a'&&x<='f'?x-'a'+10:
          x>='A'&&x<='F'?x-'A'+10:256;
        b=y>='0'&&y<='9'?y-'0':y>='a'&&y<='f'?y-'a'+10:
          y>='A'&&y<='F'?y-'A'+10:256;
        if(a>15||b>15)return -1;
        output[i]=(uint8_t)((a<<4)|b);nonzero|=output[i];
    }
    return nonzero?0:-1;
}
static struct mock_image *find_image(const uint8_t value[16]){
    for(unsigned i=0;i<sizeof images/sizeof images[0];i++)
        if(!memcmp(images[i].lease,value,16))return &images[i];
    return NULL;
}
/* drmasahi.c calls this from DRM_IOCTL_PRIME_FD_TO_HANDLE to find the backing
 * IOSurface of an imported carrier fd. The fd is matched by fstat identity
 * against each issued carrier's recorded (st_dev, st_ino); only the in-process
 * metal fixture keeps the surface here, so a socket-broker import resolves to
 * NULL and reports resolved=0. */
IOSurfaceRef rdgpu_mock_gate_resolve_fd(int fd){
    if(!metal_fixture||fd<0)return NULL;
    for(unsigned i=0;i<image_count;i++){
        struct mock_image *m=&images[i];
        if(m->carrier&&m->retainedSurface&&
           rd_carrier_matches_fd(m->carrier,fd))
            return m->retainedSurface;
    }
    return NULL;
}
static RDGPUReply response(const RDGPURequest *q){
    RDGPUReply r={.magic=RDGPU_WIRE_MAGIC,.version=RDGPU_WIRE_VERSION,
                  .bytes=RDGPU_WIRE_BYTES,.request_id=q->request_id};
    memcpy(r.lease,q->lease,16);return r;
}
static bool exact_fixture_peer(pid_t pid,void *ctx){
    return ctx&&pid==*(pid_t *)ctx;
}
static int socket_roundtrip(RDGPUSocketCommand *command,
                            RDGPUSocketResponse *reply,int *received_fd){
    if(socket_fixture<0||!command||!reply)return -1;
    command->magic=RDGPU_SOCKET_MAGIC;
    command->version=RDGPU_SOCKET_VERSION;
    command->bytes=sizeof *command;
    command->sequence=++socket_sequence;
    size_t sent=0;
    while(sent<sizeof *command){
        ssize_t count=send(socket_fixture,(const char *)command+sent,
                           sizeof *command-sent,0);
        if(count<=0)return -1;
        sent+=(size_t)count;
    }
    char control[CMSG_SPACE(sizeof(int))]={0};
    struct iovec io={reply,sizeof *reply};
    struct msghdr message={.msg_iov=&io,.msg_iovlen=1,
                           .msg_control=control,.msg_controllen=sizeof control};
    ssize_t got=recvmsg(socket_fixture,&message,0);
    if(got<=0||(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC)))return -1;
    size_t have=(size_t)got;
    while(have<sizeof *reply){
        ssize_t count=recv(socket_fixture,(char *)reply+have,sizeof *reply-have,0);
        if(count<=0)return -1;
        have+=(size_t)count;
    }
    if(have!=sizeof *reply||
       reply->magic!=RDGPU_SOCKET_MAGIC||
       reply->version!=RDGPU_SOCKET_VERSION||reply->bytes!=sizeof *reply||
       reply->kind!=command->kind||reply->sequence!=command->sequence)return -1;
    int fd=-1;
    for(struct cmsghdr *c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(&message,c))
        if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS&&
           c->cmsg_len==CMSG_LEN(sizeof(int)))memcpy(&fd,CMSG_DATA(c),sizeof fd);
    if(fd>=0){
        int flags=fcntl(fd,F_GETFD);
        if(flags<0||fcntl(fd,F_SETFD,flags|FD_CLOEXEC)<0){close(fd);return -1;}
    }
    if(received_fd)*received_fd=fd;
    else if(fd>=0)close(fd);
    return 0;
}
static void fixture_cleanup(void){
    for(unsigned i=0;i<sizeof images/sizeof images[0];i++){
        struct mock_image *m=&images[i];
        if(m->device&&m->inode){rdgpu_handoff_revoke(m->device,m->inode);m->device=0;m->inode=0;}
        if(m->carrier){rd_carrier_destroy(m->carrier);m->carrier=NULL;}
        if(m->retainedSurface&&metal_fixture){
            rdgpu_test_return_surface(metal_fixture,m->lease,1,m->retainedSurface);
            m->retainedSurface=NULL;
        }
    }
    if(metal_fixture){rdgpu_session_disconnect(metal_fixture);metal_fixture=NULL;}
}
static IOSurfaceRef fixture_resolve(uint64_t handle,void *context){
    (void)context;
    for(unsigned i=0;i<image_count;i++)
        if(images[i].sequence==handle&&metal_fixture)
            return rdgpu_test_export_surface(metal_fixture,images[i].lease,1);
    return NULL;
}
static long fixture_draw_submit(void *context,const RDGPUDrawBatchRequest *header,
                                const void *packet,size_t bytes){
    (void)context;
    if(!metal_fixture||!header||!packet||!bytes)return -EPERM;
    struct mock_image *m=find_image(header->lease);
    if(!m||header->generation!=1||!header->target_handle||
       header->packet_bytes!=bytes)return -EPERM;
    /* The resolver's handle is scoped to this exact lease. This synchronous
     * Metal execution is fixture-only; the real broker needs async fences. */
    m->sequence=header->target_handle;
    int result=rdgpu_draw_execute(packet,bytes,fixture_resolve,NULL);
    fprintf(stderr,"RDMETAL_FIXTURE_DRAW result=%d bytes=%zu\n",result,bytes);
    if(result!=RDGPU_DRAW_OK)return -EIO;
    draw_fence_done=++draw_fence_next;
    return (long)draw_fence_done;
}
static int fixture_draw_poll(void *context,uint64_t fence){
    (void)context;return fence&&fence==draw_fence_done?0:-ESTALE;
}
static RDGPUReply mock_transport(void *opaque,const RDGPURequest *q){
    (void)opaque;RDGPUReply r=response(q);
    if(socket_fixture>=0){
        RDGPUSocketCommand command={.kind=RDGPU_SOCKET_REQUEST,.request=*q};
        RDGPUSocketResponse answer={0};
        if(socket_roundtrip(&command,&answer,NULL)||answer.status){
            r.status=RDGPU_DENIED;return r;
        }
        r=answer.reply;
        if(r.status==RDGPU_OK&&q->op==RDGPU_CREATE_BGRA8&&
           image_count<sizeof images/sizeof images[0]){
            struct mock_image *m=&images[image_count++];
            memcpy(m->lease,r.lease,16);
            m->width=(uint32_t)q->arg0;m->height=(uint32_t)(q->arg0>>32);
        }
        return r;
    }
    if(metal_fixture){
        if(q->request_id>UINT64_MAX-broker_request_offset){
            r.status=RDGPU_BAD_MESSAGE;return r;
        }
        RDGPURequest host=*q;
        host.request_id+=broker_request_offset;
        r=rdgpu_dispatch(metal_fixture,&host);
        if(q->op==RDGPU_WAIT){
            for(unsigned attempt=0;attempt<200&&r.status==RDGPU_PENDING;attempt++){
                usleep(1000);
                if(host.request_id==UINT64_MAX)break;
                host.request_id++;broker_request_offset++;
                r=rdgpu_dispatch(metal_fixture,&host);
            }
        }
        r.request_id=q->request_id; /* host-only synthetic polls are invisible */
        if(q->op==RDGPU_CREATE_BGRA8||q->op==RDGPU_WAIT)
            fprintf(stderr,"RDMETAL_FIXTURE_BROKER op=%u status=%u generation=%llu fence=%llu\n",
                    q->op,r.status,(unsigned long long)r.value1,
                    (unsigned long long)r.value0);
        if(r.status==RDGPU_OK&&q->op==RDGPU_CREATE_BGRA8){
            /* Reuse a retired slot so a bounded frame pool can run many
             * frames instead of being capped at RDGPU_MAX_IMAGES_PER_PEER. */
            struct mock_image *m=NULL;
            unsigned slot=0;
            for(;slot<sizeof images/sizeof images[0];slot++)
                if(!images[slot].lease[0]){m=&images[slot];break;}
            if(m){
                memset(m,0,sizeof *m);
                memcpy(m->lease,r.lease,16);
                m->width=(uint32_t)q->arg0;
                m->height=(uint32_t)(q->arg0>>32);
                if(slot+1>image_count)image_count=slot+1;
            }
        }
        if(r.status==RDGPU_OK&&q->op==RDGPU_CLEAR){
            struct mock_image *m=find_image(q->lease);
            if(m){m->expectedClear=q->arg0;m->hasExpectedClear=1;}
        }
        if(r.status==RDGPU_OK&&q->op==RDGPU_RELEASE){
            struct mock_image *m=find_image(q->lease);
            if(m){
                if(m->device&&m->inode){
                    rdgpu_handoff_revoke(m->device,m->inode);
                    m->device=0;m->inode=0;
                }
                if(m->carrier){rd_carrier_destroy(m->carrier);m->carrier=NULL;}
                m->exported=0;
                m->hasExpectedClear=0;
                memset(m->lease,0,16);
            }
        }
        return r;
    }
    switch(q->op){
      case RDGPU_CREATE_BGRA8:
        if(image_count>=image_limit()){
            r.status=RDGPU_BAD_STATE;break;
        }
        struct mock_image *m=&images[image_count++];
        arc4random_buf(m->lease,16);
        m->width=(uint32_t)q->arg0;
        m->height=(uint32_t)(q->arg0>>32);
        memcpy(r.lease,m->lease,16);
        r.value1=1;break;
      case RDGPU_CLEAR:
        if(!find_image(q->lease))r.status=RDGPU_BAD_STATE;
        else {
            r.value0=1;
            if(clear_count++<2)
                fprintf(stderr,"PRIVATE_BROKER_CLEAR ordinal=%u rgba16=%llx\n",
                        clear_count,(unsigned long long)q->arg0);
        }
        break;
      case RDGPU_WAIT:break;
      case RDGPU_RELEASE:r.status=RDGPU_BAD_STATE;break;
      default:r.status=RDGPU_BAD_MESSAGE;break;
    }
    return r;
}
static int authorize(void *opaque,pid_t pid,const uint8_t expected[16],uint64_t generation){
    (void)opaque;struct mock_image *m=find_image(expected);
    return m&&!m->exported&&pid==getpid()&&generation==1;
}
static int issue(void *opaque,pid_t pid,const RDGPURequest *q,
                 RDGPUReply *r,RDGPUExportImage *image){
    (void)opaque;
    struct mock_image *m=find_image(q->lease);
    if(pid!=getpid()||q->op!=RDGPU_PREPARE_EXPORT||q->arg1!=1||
       !m||m->exported)return -EPERM;
    *r=response(q);r->value0=0xabc;
    if(seq==0)memcpy(image->window_token,issued_token,16);
    else if(seq==1&&have_token2)memcpy(image->window_token,issued_token2,16);
    else arc4random_buf(image->window_token,16);
    image->sequence=++seq;
    image->width=m->width;image->height=m->height;
    image->stride=m->width*4;
    image->format=RDGPU_EXPORT_FORMAT_ARGB8888;
    memcpy(m->token,image->window_token,16);
    m->sequence=image->sequence;m->exported=1;
    return 0;
}
static void abort_export(void *opaque,pid_t pid,const uint8_t token[16],uint64_t sequence){
    (void)opaque;(void)pid;
    for(unsigned i=0;i<image_count;i++)
        if(images[i].sequence==sequence&&
           !memcmp(images[i].token,token,16))images[i].exported=0;
}
static long mock_fd_export(void *,pid_t,const uint8_t [16],uint64_t,
                           uint64_t,uint64_t,uint32_t,uint32_t,uint32_t);
static int mock_fd_import(void *ctx,pid_t actual,int fd,uint8_t lease_out[16]);
static long mock_stride(void *ctx,pid_t actual,const uint8_t lease[16],
                         uint64_t generation,uint32_t width,uint32_t height){
    (void)ctx;
    struct mock_image *m=find_image(lease);
    if(socket_fixture>=0){
        if(actual!=getpid()||!m||generation!=1||
           m->width!=width||m->height!=height){
            fprintf(stderr,"MOCK_STRIDE_DENY actual=%d pid=%d m=%p gen=%llu w=%u mw=%u h=%u mh=%u\n",
                (int)actual,getpid(),(void*)m,(unsigned long long)generation,
                width,m?m->width:0u,height,m?m->height:0u);
            return -EPERM;
        }
        RDGPUSocketCommand q={.kind=RDGPU_SOCKET_STRIDE};
        q.image=(RDGPUFDIssueWire){.magic=RDGPU_FD_ISSUE_MAGIC,
            .version=RDGPU_FD_ISSUE_VERSION,.bytes=sizeof(RDGPUFDIssueWire),
            .generation=generation,.width=width,.height=height,
            .fourcc=RDGPU_FD_ISSUE_ARGB8888};
        memcpy(q.image.lease,lease,16);
        RDGPUSocketResponse r={0};
        int rt=socket_roundtrip(&q,&r,NULL);
        fprintf(stderr,"MOCK_STRIDE_SOCKET rt=%d status=%u stride=%u\n",rt,r.status,r.stride);
        return rt||r.status?-EIO:(long)r.stride;
    }
    if(metal_fixture){
        if(actual!=getpid()||!m||generation!=1||m->width!=width||
           m->height!=height){fprintf(stderr,"RDMETAL_FIXTURE_STRIDE_DENY identity\n");return -EPERM;}
        IOSurfaceRef image=rdgpu_test_export_surface(metal_fixture,lease,generation);
        if(!image){fprintf(stderr,"RDMETAL_FIXTURE_STRIDE_FALLBACK width=%u\n",width);return width<=2048?(long)width*4:-EPERM;}
        long stride=(long)IOSurfaceGetBytesPerRow(image);
        rdgpu_test_return_surface(metal_fixture,lease,generation,image);
        fprintf(stderr,"RDMETAL_FIXTURE_STRIDE_OK stride=%ld\n",stride);
        return stride;
    }
    return actual==getpid()&&m&&generation==1&&m->width==width&&
           m->height==height&&width<=2048 ? (long)width*4 : -EPERM;
}
static void init(void){
    const char *socketPath=getenv("RDMETAL_SOCKET_BROKER");
    if(socketPath){
        if(strlen(socketPath)>=sizeof(((struct sockaddr_un *)0)->sun_path)){
            init_failed=1;return;
        }
        socket_fixture=socket(AF_UNIX,SOCK_STREAM,0);
        struct sockaddr_un address={.sun_family=AF_UNIX};
        if(socket_fixture<0){init_failed=1;return;}
        strcpy(address.sun_path,socketPath);
        if(connect(socket_fixture,(void *)&address,sizeof address)){
            close(socket_fixture);socket_fixture=-1;init_failed=1;return;
        }
    }
    const char *metalOption=getenv("RDMETAL_SAME_IMAGE_METAL_FIXTURE");
    if(metalOption&&!strcmp(metalOption,"1")){
        static pid_t fixture_pid;
        fixture_pid=getpid();
        metal_fixture=rdgpu_test_session_create(fixture_pid,
                                                 exact_fixture_peer,&fixture_pid);
        if(!metal_fixture){init_failed=1;return;}
        atexit(fixture_cleanup);
    }
    const char *hex=getenv("RDMETAL_LEDGER_MOCK_TOKEN_HEX");
    if(hex){
        if(parse_token(hex,issued_token)){init_failed=1;return;}
    }else arc4random_buf(issued_token,16);
    const char *second=getenv("RDMETAL_LEDGER_MOCK_TOKEN2_HEX");
    if(second){
        if(parse_token(second,issued_token2)||
           !memcmp(issued_token2,issued_token,16)){init_failed=1;return;}
        have_token2=1;
    }
    rdgpu_guest_state_init(&state,getpid(),mock_transport,NULL);
    rdgpu_guest_set_export_authorizer(&state,authorize,NULL);
    rdgpu_guest_set_export_transport(&state,issue,abort_export,
                                    gmem_guest_export_copyout,NULL);
    if(metal_fixture)
        rdgpu_guest_set_draw_transport(&state,fixture_draw_submit,
                                       fixture_draw_poll,NULL);
    rdgpu_guest_set_fd_export_transport(&state,mock_fd_export,NULL);
    rdgpu_guest_set_fd_import_transport(&state,mock_fd_import,NULL);
    rdgpu_guest_set_image_stride_transport(&state,mock_stride,NULL);
}
static void mock_return(void *context,const uint8_t lease[16],uint64_t generation){
    (void)context;(void)generation;
    struct mock_image *m=find_image(lease);
    if(m&&m->device&&m->inode)
        rdgpu_handoff_revoke(m->device,m->inode);
}
/* A single host-issued, exact-lease FD carrier for the mock test.  The
 * compositor must still validate SCM_RIGHTS provenance; a guest FD number
 * alone grants no IOSurface access. */
static long mock_fd_export(void *ctx,pid_t actual,const uint8_t lease[16],
                           uint64_t generation,uint64_t handle,uint64_t fence,
                           uint32_t width,uint32_t height,uint32_t stride){
    (void)ctx;
    if(actual!=getpid()||!lease||!handle||!width||!height||
       generation!=1||width>2048||height>2048)return -EPERM;
    struct mock_image *m=find_image(lease);
    if(!m||m->carrier||m->width!=width||m->height!=height){
        fprintf(stderr,"MOCK_FD_EXPORT_DENY m=%p carrier=%d mw=%u mh=%u w=%u h=%u stride=%u gen=%llu fence=%llu\n",
                (void*)m,m?m->carrier!=NULL:-1,m?m->width:0u,m?m->height:0u,
                width,height,stride,(unsigned long long)generation,(unsigned long long)fence);
        return -EALREADY;
    }
    if(socket_fixture>=0){
        RDGPUSocketCommand q={.kind=RDGPU_SOCKET_FD_EXPORT};
        q.image=(RDGPUFDIssueWire){.magic=RDGPU_FD_ISSUE_MAGIC,
            .version=RDGPU_FD_ISSUE_VERSION,.bytes=sizeof(RDGPUFDIssueWire),
            .generation=generation,.completedFence=fence,
            .width=width,.height=height,.stride=stride,
            .fourcc=RDGPU_FD_ISSUE_ARGB8888};
        memcpy(q.image.lease,lease,16);
        RDGPUSocketResponse r={0};int received=-1;
        if(socket_roundtrip(&q,&r,&received)||r.status||received<0){
            if(received>=0)close(received);return -EIO;
        }
        fprintf(stderr,"RDMESA_SOCKET_CARRIER_ISSUED exact_lease=1 fd=%d\n",received);
        return received;
    }
    IOSurfaceRef image=NULL;
    if(metal_fixture){
        image=rdgpu_test_export_surface(metal_fixture,lease,generation);
        if(image&&(IOSurfaceGetWidth(image)!=width||IOSurfaceGetHeight(image)!=height||
           IOSurfaceGetBytesPerRow(image)!=stride||
           IOSurfaceGetPixelFormat(image)!=0x42475241u)){
            rdgpu_test_return_surface(metal_fixture,lease,generation,image);
            image=NULL;return -EINVAL;
        }
        if(image&&m->hasExpectedClear){
            if(IOSurfaceLock(image,kIOSurfaceLockReadOnly,NULL)!=kIOReturnSuccess){
                rdgpu_test_return_surface(metal_fixture,lease,generation,image);
                return -EIO;
            }
            const uint8_t *pixel=IOSurfaceGetBaseAddress(image);
            uint8_t expected[4]={
                (uint8_t)(((m->expectedClear>>32)&0xffffu)/257u),
                (uint8_t)(((m->expectedClear>>16)&0xffffu)/257u),
                (uint8_t)((m->expectedClear&0xffffu)/257u),
                (uint8_t)(((m->expectedClear>>48)&0xffffu)/257u)};
            int good=pixel!=NULL;
            for(unsigned i=0;i<4&&good;i++){
                int diff=(int)pixel[i]-(int)expected[i];
                if(diff<-1||diff>1)good=0;
            }
            if(pixel)fprintf(stderr,
                "RDMETAL_FIXTURE_PIXEL bgra=%u,%u,%u,%u expected=%u,%u,%u,%u match=%d\n",
                pixel[0],pixel[1],pixel[2],pixel[3],
                expected[0],expected[1],expected[2],expected[3],good);
            if(pixel){
                size_t row=IOSurfaceGetBytesPerRow(image);
                const uint8_t *gear=pixel+(size_t)(m->height/2)*row+
                    (size_t)(m->width*3/4)*4;
                fprintf(stderr,
                    "RDMETAL_FIXTURE_GEAR_PIXEL bgra=%u,%u,%u,%u\n",
                    gear[0],gear[1],gear[2],gear[3]);
            }
            IOSurfaceUnlock(image,kIOSurfaceLockReadOnly,NULL);
            if(!good){rdgpu_test_return_surface(metal_fixture,lease,generation,image);
                return -EIO;}
        }
    }else if(stride!=width*4)return -EINVAL;
    RDCarrier *table=rd_carrier_create(getpid(),1,mock_return,NULL);
    RDCarrierFormat format={(uint32_t)width,(uint32_t)height,
                            stride,RDCARRIER_ARGB8888,0};
    RDCarrierCapability capability={handle,generation};
    int fd=-1;
    if(!table||!rd_carrier_issue(table,lease,generation,
                                format,1,capability,&fd)||
       !rd_carrier_complete(table,lease,generation,1)){
        fprintf(stderr,"MOCK_FD_EXPORT_CARRIER_FAIL table=%p fd=%d stride=%u w=%u h=%u\n",
                (void*)table,fd,stride,width,height);
        rd_carrier_destroy(table);
        if(image)rdgpu_test_return_surface(metal_fixture,lease,generation,image);
        return -EIO;
    }
    m->carrier=table;m->retainedSurface=image;
    if(image){
        struct stat st;
        if(!fstat(fd,&st)&&st.st_dev&&st.st_ino){
            m->device=st.st_dev;m->inode=st.st_ino;
            int pub=rdgpu_handoff_publish(image,st.st_dev,st.st_ino);
            fprintf(stderr,"RDMESA_MOCK_CARRIER_HANDOFF publish=%d dev=%d ino=%llu id=%u\n",
                    pub,(int)st.st_dev,(unsigned long long)st.st_ino,
                    IOSurfaceGetID(image));
        }
        /* The handoff registry now CFRetains the surface for the compositor's
         * cross-process import. Return the broker's transient export reference
         * so a later guest RELEASE can retire the broker slot (per-frame
         * lease) instead of leaking one export ref per frame. */
        rdgpu_test_return_surface(metal_fixture,lease,generation,image);
        m->retainedSurface=NULL;
    }
    fprintf(stderr,"RDMESA_MOCK_CARRIER_ISSUED exact_lease=1 metal_image=%d\n",
            image!=NULL);
    return fd;
}
/* op 12 IMPORT_FD: resolve a re-imported dma-buf carrier fd back to the broker
 * lease of the exact image the guest just exported. The fd's fstat(st_dev,
 * st_ino) is matched against each issued carrier (rd_carrier_matches_fd, the
 * same identity check the cross-process importer uses), so the guest syscall
 * layer can map the lease back to the original opaque handle and the driver
 * reuses the SAME IOSurface-backed resource instead of allocating a fresh one.
 * A socket-broker import has no local carrier table and fails closed (ESTALE). */
static int mock_fd_import(void *ctx,pid_t actual,int fd,uint8_t lease_out[16]){
    (void)ctx;
    if(actual!=getpid()||fd<0||!lease_out)return -EPERM;
    for(unsigned i=0;i<sizeof images/sizeof images[0];i++){
        struct mock_image *m=&images[i];
        if(m->carrier&&rd_carrier_matches_fd(m->carrier,fd)){
            memcpy(lease_out,m->lease,16);
            fprintf(stderr,"RDMESA_MOCK_FD_IMPORT_OK fd=%d dev=%d ino=%llu\n",
                    fd,(int)m->device,(unsigned long long)m->inode);
            return 0;
        }
    }
    fprintf(stderr,"RDMESA_MOCK_FD_IMPORT_DENY fd=%d\n",fd);
    return -ESTALE;
}
long rdgpu_mock_gate(uint64_t op,uint64_t a1,uint64_t a2,uint64_t a3,
                     uint64_t a4,uint64_t a5){
    const char *enabled=getenv("RDMETAL_LEDGER_MOCK");
    if(!enabled||strcmp(enabled,"1"))return -ENOSYS;
    pthread_once(&once,init);
    if(init_failed)return -EPERM;
    return rdgpu_guest_syscall(&state,op,a1,a2,a3,a4,a5);
}
