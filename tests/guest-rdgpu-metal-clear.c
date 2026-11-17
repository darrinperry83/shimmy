/* A Linux guest checks the default syscall route, without fixture flags. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    long handle=syscall(0x2002,1,16,16,0,0,0);
    if(handle<=0){perror("rdgpu create");return 1;}
    const uint64_t rgba=0xffff000080004000ULL;
    long fence=syscall(0x2002,2,handle,rgba,0,0,0);
    if(fence<=0){perror("rdgpu clear");return 2;}
    for(unsigned i=0;i<200;i++){
        long result=syscall(0x2002,3,handle,fence,0,0,0);
        if(result==0)break;
        if(result!=-1||errno!=EAGAIN){perror("rdgpu wait");return 3;}
        if(i==199){fputs("rdgpu wait timed out\n",stderr);return 4;}
        usleep(1000);
    }
    if(syscall(0x2002,4,handle,0,0,0,0)){perror("rdgpu release");return 5;}
    puts("RDGPU_LOCAL_METAL_CLEAR_OK");
    return 0;
}
