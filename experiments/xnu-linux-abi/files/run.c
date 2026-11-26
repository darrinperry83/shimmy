/* Native parent validates Linux layout/content against its own Darwin/Linux stat. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
struct linux_stat {
    uint64_t dev, ino;
    uint32_t mode,nlink,uid,gid;
    uint64_t rdev,pad1;
    int64_t size;
    int32_t blksize,pad2;
    int64_t blocks,atime_sec,atime_nsec,mtime_sec,mtime_nsec,ctime_sec,ctime_nsec;
    uint32_t unused[2];
};
_Static_assert(sizeof(struct linux_stat)==128,"Linux stat size");
static pid_t child;
static void timeout(int sig) { (void)sig; if(child>0)kill(child,SIGKILL); _exit(124); }
static uint64_t linux_dev(dev_t dev)
{
    uint64_t maj=major(dev),min=minor(dev);
    return (min & 255) | ((maj & 4095)<<8) | ((min & ~255ULL)<<12) | ((maj & ~4095ULL)<<32);
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    char executable[PATH_MAX];if(!realpath(argv[1],executable))return 2;
    signal(SIGALRM,timeout);alarm(20);
    pid_t parent=getpid();
    char directory[]="/tmp/rdxnu-files-XXXXXX";
    if(!mkdtemp(directory))return 2;
    char path[PATH_MAX],linkpath[PATH_MAX];
    snprintf(path,sizeof(path),"%s/data",directory);
    snprintf(linkpath,sizeof(linkpath),"%s/link",directory);
    int file=open(path,O_CREAT|O_EXCL|O_RDWR,0600);
    if(file<0||fchmod(file,0640)||symlink("data",linkpath))return 2;
    unsigned char seed[32768];memset(seed,0x41,16384);memset(seed+16384,0x42,16384);
    const uint32_t code[]={0x528004a0,0xd65f03c0};memcpy(seed+16384,code,sizeof(code));
    size_t written=0;
    while(written<sizeof(seed)) {
        ssize_t n=write(file,seed+written,sizeof(seed)-written);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return 2;
        written+=(size_t)n;
    }
    struct stat expected;
    if(fstat(file,&expected)||expected.st_size!=32768)return 2;
    close(file);
    int channel[2];if(pipe(channel))return 2;
    int dir=open(directory,O_RDONLY|O_DIRECTORY);if(dir<0)return 2;
    child=fork();if(child<0)return 2;
    if(!child){
        struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
        close(channel[0]);
        if(dup2(channel[1],1)<0)_exit(100);
        close(channel[1]);
        if(fchdir(dir)||dup2(dir,3)<0)_exit(100);
        if(dir>3)close(dir);
        char *args[]={executable,path,NULL},*env[]={NULL};
        execve(executable,args,env);perror("files ELF exec");_exit(101);
    }
    close(channel[1]);close(dir);
    unsigned char output[1024]={0};size_t used=0;
    while(used<sizeof(output)){
        ssize_t n=read(channel[0],output+used,sizeof(output)-used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)break;
        used+=(size_t)n;
    }
    close(channel[0]);int status=0;
    while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 2;
    alarm(0);
    unlink(linkpath);unlink(path);rmdir(directory);
    printf("LINUX_FILES_RESULT status=%d native_parent=%d bytes=%zu\n",status,getpid(),used);
    if(!WIFEXITED(status)||WEXITSTATUS(status)||getpid()!=parent||used!=128+sizeof("LINUX_FILES_PASS\n")-1)return 1;
    struct linux_stat stat;memcpy(&stat,output,sizeof(stat));
    if(memcmp(output+128,"LINUX_FILES_PASS\n",sizeof("LINUX_FILES_PASS\n")-1)||
       stat.dev!=linux_dev(expected.st_dev)||stat.ino!=expected.st_ino||
       stat.mode!=expected.st_mode||stat.uid!=expected.st_uid||stat.gid!=expected.st_gid||
       stat.nlink!=expected.st_nlink||stat.rdev!=linux_dev(expected.st_rdev)||
       stat.size!=expected.st_size||stat.blocks!=expected.st_blocks||stat.blksize!=expected.st_blksize)return 1;
    puts("XNU_FILES_PERSONALITY_PASS: open/stat/read/map/close, paths, symlinks, offsets, errno; native metadata and parent intact");
    return 0;
}
