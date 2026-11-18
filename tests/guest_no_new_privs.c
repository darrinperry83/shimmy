#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static void *thread_check(void *unused) {
    (void)unused;
    return (void *)(long)(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1);
}

int main(int argc, char **argv) {
    if (getenv("RDSECCOMP_FD")) return 1; /* host capability stays private */
    if (argc > 1) {
        if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) return 2;
        puts("GUEST_NNP_EXEC_OK");
        return 0;
    }
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 0) return 3;
    errno = 0;
    if (prctl(PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0) != -1 || errno != EINVAL) return 4;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return 5;
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) return 6;
    pthread_t thread;
    void *thread_result = (void *)1;
    if (pthread_create(&thread, NULL, thread_check, NULL) != 0 ||
        pthread_join(thread, &thread_result) != 0 || thread_result != NULL) return 13;
    errno = 0;
    if (prctl(PR_GET_NO_NEW_PRIVS, 1, 0, 0, 0) != -1 || errno != EINVAL) return 7;
    pid_t child = fork();
    if (child < 0) return 8;
    if (child == 0) {
        if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) _exit(9);
        if (setenv("RDSECCOMP_FD", "1", 1) != 0) _exit(10);
        execl("/tmp/guest-nnp", "/tmp/guest-nnp", "exec", (char *)NULL);
        _exit(12);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
        return 11;
    puts("GUEST_NNP_FORK_EXEC_OK");
    return 0;
}
