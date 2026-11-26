/* Drive the disposable VM's guarded recovery script through a native PTY. */
#include <sys/sysctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <util.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int send_line(int fd, const char *text) {
    size_t length = strlen(text);
    while (length) {
        ssize_t n = write(fd, text, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        text += n;
        length -= (size_t)n;
    }
    return write(fd, "\n", 1) == 1 ? 0 : -1;
}

int main(int argc, char **argv) {
    int virtualized = 0;
    size_t size = sizeof virtualized;
    if (sysctlbyname("kern.hv_vmm_present", &virtualized, &size, NULL, 0) || !virtualized) {
        fputs("REFUSED: VM recovery authorization only\n", stderr);
        return 1;
    }
    if (argc != 3 || strlen(argv[1]) > 128 || strlen(argv[2]) > 256 ||
        strpbrk(argv[1], "\r\n") || strpbrk(argv[2], "\r\n")) return 2;
    int terminal;
    struct termios settings;
    if (tcgetattr(STDIN_FILENO, &settings)) return 1;
    settings.c_lflag &= ~ECHO;
    pid_t child = forkpty(&terminal, NULL, &settings, NULL);
    if (child < 0) { perror("forkpty"); return 1; }
    if (!child) {
        execl("/bin/sh", "sh", "/tmp/rdxnu/recovery.sh", "configure", NULL);
        _exit(127);
    }
    char pending[8192] = {0}, chunk[1024];
    size_t used = 0;
    int stage = 0, failed = 0;
    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(terminal, &readable);
        struct timeval timeout = {.tv_sec = 90};
        int ready = select(terminal + 1, &readable, NULL, NULL, &timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) {
            fputs("AUTH_PTY_TIMEOUT_OR_ERROR\n", stderr);
            failed = 1;
            break;
        }
        ssize_t n = read(terminal, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        fwrite(chunk, 1, (size_t)n, stdout);
        fflush(stdout);
        if (used + (size_t)n >= sizeof pending) used = 0;
        memcpy(pending + used, chunk, (size_t)n);
        used += (size_t)n;
        pending[used] = 0;
        const char *response = NULL;
        if (stage == 0 && strstr(pending, "enter y or n")) response = "y";
        else if (stage == 1 && strstr(pending, "Username:")) response = argv[1];
        else if (stage == 2 && strstr(pending, "Password:")) response = argv[2];
        else if (stage >= 3 && (strstr(pending, "Username:") || strstr(pending, "Password:"))) {
            failed = 1;
            break;
        }
        if (response) {
            /* Let the CLI finish changing terminal modes before replying. */
            usleep(300000);
            if (send_line(terminal, response)) { failed = 1; break; }
            stage++;
            used = 0;
            pending[0] = 0;
        }
    }
    if (failed) kill(-child, SIGTERM);
    close(terminal);
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return 1;
    if (failed || !WIFEXITED(status)) return 1;
    return WEXITSTATUS(status);
}
