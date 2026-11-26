#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern int64_t rdxnu_probe_syscall(uint64_t nr);
extern int64_t rdxnu_darwin_getpid(void);

struct results {
    int64_t expected_pid, darwin_pid, linux_pid, linux_unknown;
};

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "--expect-stock") &&
                      strcmp(argv[1], "--expect-kernel"))) {
        fprintf(stderr, "usage: %s --expect-stock|--expect-kernel\n", argv[0]);
        return 2;
    }
    int expect_kernel = !strcmp(argv[1], "--expect-kernel");
    int channel[2];
    if (pipe(channel)) { perror("pipe"); return 2; }
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 2; }
    if (!child) {
        close(channel[0]);
        struct rlimit limit = {0, 0};
        setrlimit(RLIMIT_CORE, &limit);
        /* Stock XNU may terminate this child with SIGSYS. No handler is used. */
        struct results r;
        r.expected_pid = getpid();
        r.darwin_pid = rdxnu_darwin_getpid();
        if (r.darwin_pid != r.expected_pid) _exit(3);
        r.linux_pid = rdxnu_probe_syscall(172);
        r.linux_unknown = rdxnu_probe_syscall(UINT64_MAX);
        _exit(write(channel[1], &r, sizeof(r)) == sizeof(r) ? 0 : 2);
    }
    close(channel[1]);
    struct results r = {0};
    size_t received = 0;
    while (received < sizeof(r)) {
        ssize_t n = read(channel[0], (char *)&r + received, sizeof(r) - received);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        received += (size_t)n;
    }
    close(channel[0]);
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("waitpid"); return 2;
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        printf("PROBE_CHILD_SIGNAL=%d\n", sig);
        if (!expect_kernel && sig == SIGSYS) {
            puts("STOCK_NEGATIVE_CONTROL_PASS: marked Linux entry unavailable");
            return 0;
        }
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) || received != sizeof(r)) {
        fputs("probe transport failed\n", stderr);
        return 2;
    }
    printf("pid=%" PRId64 " darwin=%" PRId64 " linux=%" PRId64
           " unknown=%" PRId64 "\n", r.expected_pid, r.darwin_pid,
           r.linux_pid, r.linux_unknown);
    if (r.darwin_pid != r.expected_pid) {
        fputs("Darwin control failed\n", stderr);
        return 1;
    }
    if (!expect_kernel && (r.linux_pid != ENOSYS || r.linux_unknown != ENOSYS)) {
        fputs("unexpected stock-kernel result; refusing to call this a pass\n", stderr);
        return 1;
    }
    int supported = r.linux_pid == r.expected_pid && r.linux_unknown == -38;
    if (supported != expect_kernel) {
        fputs("result disagrees with expected kernel capability\n", stderr);
        return 1;
    }
    puts(supported ? "KERNEL_LINUX_ABI_P0_PASS" :
         "STOCK_NEGATIVE_CONTROL_PASS: marked Linux entry unavailable");
    return 0;
}
