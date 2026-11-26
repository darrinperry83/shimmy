/* Native parent: exec the actual ELF and validate child identity/output/exit. */
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    pid_t control = getpid();
    int channel[2];
    if (pipe(channel)) return 2;
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) {
        struct rlimit limit = {0,0};
        setrlimit(RLIMIT_CORE, &limit);
        close(channel[0]);
        if (dup2(channel[1], 1) < 0) _exit(100);
        close(channel[1]);
        char *args[] = {argv[1], NULL};
        char *env[] = {NULL};
        execve(argv[1], args, env);
        perror("ELF execve");
        _exit(101);
    }
    close(channel[1]);
    struct { char magic[8]; uint64_t pid; } result = {{0},0};
    size_t n = 0;
    while (n < sizeof result) {
        ssize_t got = read(channel[0], (char *)&result + n, sizeof result - n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        n += (size_t)got;
    }
    close(channel[0]);
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return 2;
    printf("ELF_RESULT bytes=%zu child=%d reported=%llu status=%d native_parent=%d\n",
           n, child, (unsigned long long)result.pid, status, getpid());
    if (n != sizeof result || memcmp(result.magic,"RDELF001",8) ||
        result.pid != (uint64_t)child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 37 || getpid() != control) return 1;
    puts("LINUX_ELF_M1_PASS: stack, BSS, getpid, ENOSYS, EFAULT, EBADF, write, exit_group; native parent intact");
    return 0;
}
