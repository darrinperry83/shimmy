/* Native controller; Linux child owns the tested mappings, XNU owns their backing. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
static pid_t child;
static void timeout(int sig) { (void)sig; if(child>0)kill(child,SIGKILL); _exit(124); }
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    signal(SIGALRM,timeout);alarm(15);
    pid_t parent=getpid();int channel[2];
    if(pipe(channel))return 2;
    int zero=open("/dev/zero",O_RDONLY);
    char path[]="/tmp/rdxnu-filemap-XXXXXX";
    int file=mkstemp(path);
    int directory=open("/tmp",O_RDONLY);
    if(zero<0||file<0||directory<0)return 2;
    unlink(path);
    errno=0;
    void *native_map=mmap(NULL,16384,PROT_READ,MAP_PRIVATE,directory,0);
#ifdef __APPLE__
    int native_expected=EINVAL;
#else
    int native_expected=ENODEV;
#endif
    if(native_map!=MAP_FAILED||errno!=native_expected)return 2;
    unsigned char seed[32768];memset(seed,0x31,16384);memset(seed+16384,0x32,16384);
    const uint32_t code[]={0x528004a0,0xd65f03c0}; /* mov w0,#37; ret */
    memcpy(seed+16384,code,sizeof(code));
    size_t written=0;
    while(written<sizeof(seed)) {
        ssize_t n=write(file,seed+written,sizeof(seed)-written);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return 2;
        written+=(size_t)n;
    }
    if(lseek(file,0,SEEK_SET)!=0)return 2;
    child=fork();if(child<0)return 2;
    if(!child){
        struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
        close(channel[0]);
        if(dup2(channel[1],1)<0)_exit(100);
        close(channel[1]);
        if(dup2(zero,3)<0||dup2(file,4)<0)_exit(100);
        if(zero>4)close(zero);
        if(file>4)close(file);
        if(dup2(directory,5)<0)_exit(100);
        if(directory>5)close(directory);
        char *args[]={argv[1],NULL},*env[]={NULL};
        execve(argv[1],args,env);perror("filemap ELF exec");_exit(101);
    }
    close(channel[1]);close(zero);close(directory);
    char buffer[128]={0};size_t used=0;
    while(used<sizeof(buffer)-1){
        ssize_t n=read(channel[0],buffer+used,sizeof(buffer)-1-used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)break;
        used+=(size_t)n;
    }
    close(channel[0]);int status;
    while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 2;
    unsigned char actual[32768];
    if(lseek(file,0,SEEK_SET)!=0)return 2;
    size_t received=0;
    while(received<sizeof(actual)) {
        ssize_t n=read(file,actual+received,sizeof(actual)-received);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return 2;
        received+=(size_t)n;
    }
    close(file);alarm(0);
    printf("LINUX_FILEMAP_RESULT status=%d native_parent=%d output=%s",status,getpid(),buffer);
    if(!WIFEXITED(status)||WEXITSTATUS(status)||getpid()!=parent||
       strcmp(buffer,"LINUX_FILEMAP_PASS\n")||memcmp(seed,actual,sizeof(seed)))return 1;
    puts("XNU_FILEMAP_PERSONALITY_PASS: private file data, unchanged backing file, RX execution, fixed replacement, offsets, errno; native parent intact");
    return 0;
}
