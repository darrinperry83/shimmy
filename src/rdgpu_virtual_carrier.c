#include "rdgpu_virtual_carrier.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

struct RDCarrier {
    pid_t compositor_pid;
    uint64_t audit_session;
    RDCarrierReturn on_return;
    void *context;
    int keeper;
    dev_t device;
    ino_t inode;
    uint8_t lease[16];
    uint64_t generation,fence;
    RDCarrierCapability capability;
    RDCarrierFormat format;
    const void *client,*buffer;
    bool issued,complete,bound;
};
static bool format_ok(RDCarrierFormat f){
    return f.width&&f.height&&f.width<=2048&&f.height<=2048&&
        f.fourcc==RDCARRIER_ARGB8888&&f.stride>=f.width*4&&
        f.stride<=2048*4&&(uint64_t)f.stride*f.height<=16u*1024u*1024u;
}
static bool same(RDCarrierFormat a,RDCarrierFormat b){
    return a.width==b.width&&a.height==b.height&&a.stride==b.stride&&
        a.fourcc==b.fourcc&&a.modifier==b.modifier;
}
RDCarrier *rd_carrier_create(pid_t pid,uint64_t session,
                            RDCarrierReturn returned,void *context){
    if(pid<=0||!session||!returned)return NULL;
    RDCarrier *t=calloc(1,sizeof *t);
    if(t){t->compositor_pid=pid;t->audit_session=session;
          t->on_return=returned;t->context=context;t->keeper=-1;}
    return t;
}
bool rd_carrier_issue(RDCarrier *t,const uint8_t lease[16],
                      uint64_t generation,RDCarrierFormat format,
                      uint64_t fence,RDCarrierCapability capability,
                      int *carrier_fd){
    if(!t||t->issued||!lease||!generation||!fence||!carrier_fd||
       !capability.opaque||!capability.generation||
       !format_ok(format))return false;
    uint8_t any=0;for(unsigned i=0;i<16;i++)any|=lease[i];
    if(!any)return false;
    char path[]="/tmp/rdgpu-carrier-XXXXXX";
    int writer=mkstemp(path);
    if(writer<0)return false;
    /* An ordinary dma-buf importer validates plane bounds with fstat/lseek.
     * This sparse file carries identity only; it contains no image pixels. */
    if(ftruncate(writer,(off_t)format.stride*format.height)){
        unlink(path);close(writer);return false;
    }
    // The FD is only an identity carrier. No shader, lease, or IOSurface
    // content is stored in it. The broker keeps its reference until release.
    int keeper=open(path,O_RDONLY|O_CLOEXEC);
    unlink(path);close(writer);
    if(keeper<0)return false;
    struct stat st;
    if(fstat(keeper,&st)||st.st_ino==0||
       st.st_size<(off_t)format.stride*format.height){close(keeper);return false;}
    int outgoing=fcntl(keeper,F_DUPFD_CLOEXEC,0);
    if(outgoing<0){close(keeper);return false;}
    t->keeper=keeper;t->device=st.st_dev;t->inode=st.st_ino;
    memcpy(t->lease,lease,16);t->generation=generation;t->fence=fence;
    t->format=format;t->capability=capability;t->issued=true;
    *carrier_fd=outgoing;
    return true;
}
bool rd_carrier_complete(RDCarrier *t,const uint8_t lease[16],
                         uint64_t generation,uint64_t completed){
    if(!t||!t->issued||t->complete||t->bound||!lease||
       generation!=t->generation||completed!=t->fence||
       memcmp(lease,t->lease,16))return false;
    t->complete=true;return true;
}
bool rd_carrier_import(RDCarrier *t,int fd,RDCarrierFormat format,
                       uint64_t sequence,pid_t pid,uint64_t session,
                       const void *client,const void *buffer,
                       RDCarrierCapability *out){
    if(out)*out=(RDCarrierCapability){0,0};
    if(!t||!t->issued||!t->complete||t->bound||fd<0||
       !client||!buffer||!out||pid!=t->compositor_pid||session!=t->audit_session||
       sequence!=t->fence||!same(format,t->format))return false;
    struct stat st;
    if(fstat(fd,&st)||st.st_dev!=t->device||st.st_ino!=t->inode)return false;
    // A received SCM_RIGHTS fd is explicitly marked CLOEXEC by the receiver
    // before it can be admitted here. Plain dup() does not preserve this.
    int flags=fcntl(fd,F_GETFD);
    if(flags<0||!(flags&FD_CLOEXEC))return false;
    t->client=client;t->buffer=buffer;t->bound=true;
    *out=t->capability;
    return true;
}
bool rd_carrier_matches_fd(RDCarrier *t,int fd){
    if(!t||!t->issued||fd<0)return false;
    struct stat st;
    if(fstat(fd,&st))return false;
    return st.st_dev==t->device&&st.st_ino==t->inode;
}
bool rd_carrier_release(RDCarrier *t,const void *client,const void *buffer){
    if(!t||!t->bound||t->client!=client||t->buffer!=buffer)return false;
    t->on_return(t->context,t->lease,t->generation);
    close(t->keeper);t->keeper=-1;t->issued=false;t->bound=false;
    memset(t->lease,0,16);t->capability=(RDCarrierCapability){0,0};return true;
}
void rd_carrier_destroy(RDCarrier *t){
    if(!t)return;
    if(t->keeper>=0)close(t->keeper);
    free(t);
}
