/* APFS control: even Darwin's ordinary symlink open loses text after unlink. */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
int main(void)
{
    char directory[]="/tmp/rdxnu-native-link-XXXXXX",path[256],target[32];
    if(!mkdtemp(directory))return 2;
    snprintf(path,sizeof(path),"%s/link",directory);
    if(symlink("data",path))return 2;
    int fd=open(path,O_RDONLY|O_SYMLINK);
    if(fd<0)return 2;
    struct stat before,after;
    int first=freadlink(fd,target,sizeof(target));
    if(first!=4||memcmp(target,"data",4)||fstat(fd,&before))return 1;
    if(unlink(path)||symlink("other",path)||fstat(fd,&after))return 2;
    errno=0;
    int last=freadlink(fd,target,sizeof(target)),error=errno;
    close(fd);unlink(path);rmdir(directory);
    printf("DARWIN_SYMLINK_CONTROL before=%d after=%d errno=%d inode_retained=%d nlink=%u size=%lld\n",
        first,last,error,before.st_ino==after.st_ino,after.st_nlink,after.st_size);
    return !(last==-1&&error==EINVAL&&before.st_ino==after.st_ino&&after.st_nlink==0&&after.st_size==4);
}
