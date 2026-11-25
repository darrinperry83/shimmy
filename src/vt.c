// vt.c — Linux virtual consoles for the shim. See vt.h.
//
// Six text consoles (tty1..tty6), each a Darwin pty pair whose SLAVE is the guest's /dev/ttyN and
// whose MASTER the shim reads to render text and writes to deliver keyboard input. The pty's line
// discipline supplies termios (TCGETS/TCSETS, canonical mode, echo, ISIG) — the exact behaviour a
// Linux console's tty layer provides — so getty/agetty needs only the handful of keyboard/console
// ioctls handled below (KDGKBMODE/KDGKBTYPE/VT_*), which the pty cannot answer because they are
// keyboard/VT-driver ioctls, not termios ioctls.
#include "vt.h"
#include "console.h"
#include "syscalls.h"          // linux_errno_from_host
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <signal.h>
#include <limits.h>
#include "guestthread.h"
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

// --- Linux kd.h / vt.h ioctls (values fixed by the Linux UAPI) ------------------------------
#define LX_KDGKBTYPE   0x4B33
#define LX_KIOCSOUND   0x4B2F
#define LX_KDMKTONE    0x4B30
#define LX_KDSETMODE   0x4B3A
#define LX_KDGETMODE   0x4B3B
#define LX_KDGETLED    0x4B31
#define LX_KDSETLED    0x4B32
#define LX_KDGKBMODE   0x4B44
#define LX_KDSKBMODE   0x4B45
#define LX_KDGKBMETA   0x4B62
#define LX_KDSKBMETA   0x4B63
#define LX_KDGKBLED    0x4B64
#define LX_KDSKBLED    0x4B65
#define LX_GIO_FONT    0x4B60
#define LX_PIO_FONT    0x4B61
#define LX_GIO_FONTX   0x4B6B
#define LX_PIO_FONTX   0x4B6C

#define LX_VT_OPENQRY     0x5600
#define LX_VT_GETMODE     0x5601
#define LX_VT_SETMODE     0x5602
#define LX_VT_GETSTATE    0x5603
#define LX_VT_RELDISP     0x5605
#define LX_VT_ACTIVATE    0x5606
#define LX_VT_WAITACTIVE  0x5607
#define LX_VT_DISALLOCATE 0x5608
#define LX_VT_RESIZE      0x5609
#define LX_VT_RESIZEX     0x560A

#define LX_K_XLATE    0x01
#define LX_K_UNICODE  0x03
#define LX_KB_101     0x02
#define LX_VT_AUTO    0x00
#define LX_EINTR      4
#define LX_EINVAL     22
#define LX_ENOTTY     25

struct lx_vt_mode { char mode, waitv; short relsig, acqsig, frsig; };
struct lx_vt_stat { unsigned short v_active, v_signal, v_state; };

typedef struct {
    int master;             // pty master (shim-owned, never closed)
    char slave[128];        // slave path
    dev_t slave_dev;        // identity for vt_owns_fd (survives dup/close)
    ino_t slave_ino;
    console con;
} vt_unit;

static vt_unit g_vt[VT_MAX_TEXT];
static _Atomic int g_active = 1;         // 1..6 text, 7 = GUI
static _Atomic int g_graphics_seen;
static vt_present_fn g_present;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_inited;
static int g_enabled = -1;

// VT_WAITACTIVE: a waiter sleeps on g_waitactive_cond until vt_set_active() flips g_active and
// broadcasts. g_waitactive_lock guards BOTH the state change and the waiter's re-check, so a
// switch can never slip between the waiter's "already active?" test and its cond_wait (lost wakeup).
static pthread_mutex_t g_waitactive_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_waitactive_cond = PTHREAD_COND_INITIALIZER;

// Per-VT "is a guest fd currently open on this console" counter, mirroring Linux's vt_in_use()
// (vc port kref > 1). VT_GETSTATE reports a VT as allocated only while this is > 0 so logind's
// vt_is_busy() does not think tty2..6 are busy and skip spawning getty@ttyN.
static _Atomic int g_vt_open[VT_MAX_TEXT];

// /sys/class/tty/tty0/active: logind opens this once and epolls it; a VT switch must both change
// the value and wake the poller, exactly like a kernel sysfs attribute. A temp file is always
// EOF-readable under kqueue (logind would spin) and never changes, so we serve a pipe: the shim
// holds the write end and re-writes the current "ttyN\n" on every switch.
static int g_console_active_r = -1;      // read end handed to the guest
static int g_console_active_w = -1;      // write end held by the shim

static uint8_t* g_fb;                    // RGBA surface for the active console
static int g_fb_w, g_fb_h;

static int g_dump;                       // RDCONS_DUMP: dump text + non-black verdict

typedef struct { unsigned magic; int sid; int foreground; } vt_tty_state;
#define VT_TTY_MAGIC 0x56545459u

static int vt_state_fd(int n) {
    const char* boot = getenv("RDSHIM_BOOT");
    if (!boot || !*boot) return -1;
    for (const char* p = boot; *p; p++) if (*p < '0' || *p > '9') return -1;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "/tmp/rdvt-%s-%d.state", boot, n);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
        close(fd); return -1;
    }
    return fd;
}

static int vt_state_read_locked(int fd, vt_tty_state* state) {
    memset(state, 0, sizeof *state);
    ssize_t n = pread(fd, state, sizeof *state, 0);
    if (n < 0) return -1;
    if (n != sizeof *state || state->magic != VT_TTY_MAGIC) {
        memset(state, 0, sizeof *state); state->magic = VT_TTY_MAGIC;
    }
    return 0;
}

static int vt_state_update(int n, vt_tty_state* state, int write_back) {
    int fd = vt_state_fd(n);
    if (fd < 0) return -1;
    int rc = -1;
    if (flock(fd, LOCK_EX) == 0) {
        vt_tty_state current;
        if (vt_state_read_locked(fd, &current) == 0) {
            if (write_back) {
                if (state->sid) current.sid = state->sid;
                if (state->foreground) current.foreground = state->foreground;
                rc = pwrite(fd, &current, sizeof current, 0) == sizeof current ? 0 : -1;
            } else { *state = current; rc = 0; }
        }
        flock(fd, LOCK_UN);
    }
    close(fd);
    return rc;
}

int vt_claim_ctty(int fd) {
    int n = vt_number_for_fd(fd);
    if (!n) { errno = ENOTTY; return -1; }
    if (getsid(0) != getpid()) { errno = EPERM; return -1; }
    vt_tty_state state = { .magic = VT_TTY_MAGIC, .sid = (int)getsid(0), .foreground = (int)getpgrp() };
    return vt_state_update(n, &state, 1);
}

int vt_get_foreground(int fd, int* pgid) {
    int n = vt_number_for_fd(fd);
    if (!n) return -1;
    vt_tty_state state;
    if (vt_state_update(n, &state, 0) || !state.foreground) return -1;
    *pgid = state.foreground;
    return 0;
}

int vt_get_session(int fd, int* sid) {
    int n = vt_number_for_fd(fd);
    if (!n) return -1;
    vt_tty_state state;
    if (vt_state_update(n, &state, 0) || !state.sid) return -1;
    *sid = state.sid;
    return 0;
}

int vt_set_foreground(int fd, int pgid) {
    int n = vt_number_for_fd(fd);
    if (!n) { errno = ENOTTY; return -1; }
    if (pgid <= 0 || !guest_child_known(pgid)) { errno = EPERM; return -1; }
    vt_tty_state state;
    if (vt_state_update(n, &state, 0)) return -1;
    if (state.sid != (int)getsid(0)) { errno = EPERM; return -1; }
    if (getpgid((pid_t)pgid) != (pid_t)pgid || getsid((pid_t)pgid) != state.sid) {
        errno = EPERM; return -1;
    }
    vt_tty_state update = { .magic = VT_TTY_MAGIC, .foreground = pgid };
    return vt_state_update(n, &update, 1);
}

static int vt_get_foreground_fd_number(int n, int* pgid) {
    vt_tty_state state;
    if (vt_state_update(n, &state, 0) || !state.foreground) return -1;
    *pgid = state.foreground;
    return 0;
}

int vt_enabled(void) {
    if (g_enabled < 0) {
        const char* e = getenv("RDVT");
        g_enabled = !(e && *e && strcmp(e, "0") == 0);
    }
    return g_enabled;
}

int vt_active(void) {
    return atomic_load(&g_active);
}

int vt_text_input_active(void) {
    return vt_enabled() && atomic_load(&g_active) <= VT_MAX_TEXT;
}

// Create one pty pair, grant+unlock so the slave can become a controlling terminal.
static int vt_make_pty(int* master_out, char* slave_out, size_t slave_cap) {
    int m = open("/dev/ptmx", O_RDWR);
    if (m < 0) return -1;
    if (ioctl(m, TIOCPTYGRANT) != 0 || ioctl(m, TIOCPTYGNAME, slave_out) != 0 ||
        ioctl(m, TIOCPTYUNLK) != 0) {
        close(m);
        return -1;
    }
    *master_out = m;
    (void)slave_cap;
    return 0;
}

static void vt_present_active(void);

// Update /sys/class/tty/tty0/active: drain any stale line so the pipe holds only the latest value,
// then append the current active console. Best-effort: the pipe may not exist yet, and a dead
// reader (logind exited) makes the write EPIPE, which F_SETNOSIGPIPE turns into an ignored error.
static void vt_console_active_publish(void) {
    if (g_console_active_w < 0) return;
    char drain[256];
    while (read(g_console_active_r, drain, sizeof drain) > 0) {}
    char line[16];
    snprintf(line, sizeof line, "tty%d\n", (int)atomic_load(&g_active));
    (void)write(g_console_active_w, line, strlen(line));
}

static void vt_console_active_make(void) {
    if (g_console_active_r >= 0) return;
    int p[2];
    if (pipe(p) != 0) return;
    int r = p[0], w = p[1];
    fcntl(r, F_SETFL, fcntl(r, F_GETFL, 0) | O_NONBLOCK);  // drain() must never block
    fcntl(w, F_SETNOSIGPIPE, 1);                           // a closed reader is not fatal
    // systemd packs its own listen fds onto 3..8 at boot and would otherwise close these; keep
    // the shim-held ends reserved at >=256 (dup() below still yields a non-CLOEXEC guest fd).
    r = shim_fd_reserve(r);
    w = shim_fd_reserve(w);
    g_console_active_r = r;
    g_console_active_w = w;
    vt_console_active_publish();                           // seed with the current active console
}

int vt_console_active_open(void) {
    pthread_mutex_lock(&g_lock);
    vt_console_active_make();
    int fd = g_console_active_r >= 0 ? dup(g_console_active_r) : -1;
    pthread_mutex_unlock(&g_lock);
    return fd;
}

void vt_console_active_rewind(void) {
    pthread_mutex_lock(&g_lock);
    vt_console_active_publish();
    pthread_mutex_unlock(&g_lock);
}

int vt_console_active_owns_fd(int fd) {
    if (fd < 0 || g_console_active_r < 0) return 0;
    struct stat a, b;
    if (fstat(fd, &a) != 0 || fstat(g_console_active_r, &b) != 0) return 0;
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

// Central active-console mutation: the state change and the waiter broadcast happen under one lock
// so a VT_WAITACTIVE waiter can never miss the wakeup it is waiting for.
static int vt_set_active(int n) {
    int prev;
    pthread_mutex_lock(&g_waitactive_lock);
    prev = atomic_exchange(&g_active, n);
    pthread_cond_broadcast(&g_waitactive_cond);
    pthread_mutex_unlock(&g_waitactive_lock);
    if (prev != n) vt_console_active_publish();
    return prev;
}

void vt_switch_to(int n) {
    if (!vt_enabled()) return;
    if (n < 1 || n > VT_GRAPHICS) n = 1;
    int prev = vt_set_active(n);
    if (prev != n) {
        fprintf(stderr, "[vt] switch to %s console (was %d)\n",
                n == VT_GRAPHICS ? "GUI" : "text", prev);
        vt_present_active();
    }
}

void vt_graphics_frame(void) {
    if (!vt_enabled()) return;
    // Auto-select the desktop only when it first appears. Subsequent frames must
    // not undo a user's explicit Ctrl+Alt+Fn switch to a text login.
    if (!atomic_exchange(&g_graphics_seen, 1) &&
        vt_set_active(VT_GRAPHICS) != VT_GRAPHICS) {
        fprintf(stderr, "[vt] GUI frame detected -> input to GUI console\n");
    }
}

void vt_graphics_session_lost(void) {
    if (!vt_enabled()) return;
    atomic_store(&g_graphics_seen, 0);
    vt_switch_to(1);
}

void vt_feed_key(const char* utf8, size_t n) {
    if (!n) return;
    int act = atomic_load(&g_active);
    if (act < 1 || act > VT_MAX_TEXT) return;
    vt_unit* u = &g_vt[act - 1];
    if (u->master > 0) {
        if (!memchr(utf8, 3, n) && !memchr(utf8, 26, n)) {
            (void)write(u->master, utf8, n);
            return;
        }
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)utf8[i];
            int pgid = 0;
            if ((c == 3 || c == 26) && vt_get_foreground_fd_number(act, &pgid) == 0 &&
                guest_child_known(pgid) && getpgid((pid_t)pgid) == (pid_t)pgid) {
                (void)kill(-(pid_t)pgid, c == 3 ? SIGINT : SIGTSTP);
            } else {
                (void)write(u->master, &c, 1);
            }
        }
    }
}

void vt_set_present_fn(vt_present_fn fn) {
    g_present = fn;
}

static void vt_render_and_present(const console* c) {
    if (!g_fb || g_fb_w <= 0 || g_fb_h <= 0) return;
    console_render(c, g_fb, g_fb_w, g_fb_h, g_fb_w * 4);
    // RDVT_FBDUMP=1: dump the rendered RGBA framebuffer to /tmp/cons-fb.ppm (once) so the
    // console image can be inspected independently of the Metal present path.
    if (getenv("RDVT_FBDUMP")) {
        static int dumped;
        if (!dumped && console_has_content(c)) {
            dumped = 1;
            FILE* f = fopen("/tmp/cons-fb.ppm", "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", g_fb_w, g_fb_h);
                uint8_t* rgb = (uint8_t*)malloc((size_t)g_fb_w * 3);
                for (int y = 0; y < g_fb_h; y++) {
                    const uint8_t* s = g_fb + (size_t)y * g_fb_w * 4;
                    for (int x = 0; x < g_fb_w; x++) {
                        rgb[x*3+0] = s[x*4+0];
                        rgb[x*3+1] = s[x*4+1];
                        rgb[x*3+2] = s[x*4+2];
                    }
                    fwrite(rgb, 1, (size_t)g_fb_w * 3, f);
                }
                free(rgb);
                fclose(f);
                fprintf(stderr, "[vt] dumped console framebuffer to /tmp/cons-fb.ppm\n");
            }
        }
    }
    if (g_present) g_present(g_fb, (uint64_t)g_fb_w, (uint64_t)g_fb_h, 0);

    if (g_dump) {
        static char g_last_dump[8192];
        char snap[8192];
        console_text_snapshot(c, snap, sizeof snap);
        if (console_has_content(c) && strcmp(snap, g_last_dump) != 0) {
            snprintf(g_last_dump, sizeof g_last_dump, "%s", snap);
            // Non-black verdict: any lit pixel in the rendered surface.
            int nonblack = 0;
            size_t px = (size_t)g_fb_w * (size_t)g_fb_h;
            for (size_t i = 0; i < px; i++) {
                uint8_t* p = g_fb + i * 4;
                if (p[0] || p[1] || p[2]) { nonblack = 1; break; }
            }
            fprintf(stderr, "[vt] ---- console tty%d (%dx%d) ----\n%s[vt] ---- verdict=%s ----\n",
                    atomic_load(&g_active), c->cols, c->rows, snap,
                    nonblack ? "NONBLACK" : "BLACK");
            // RDCONS_DUMPFILE=<path>: mirror the snapshot to a file. systemd-as-PID-1 redirects fd 2
            // to the console mid-boot, so the stderr dump above is lost exactly when the interesting
            // messages arrive; a file append survives that redirection.
            const char* df = getenv("RDCONS_DUMPFILE");
            if (df && *df) {
                FILE* f = fopen(df, "a");
                if (f) { fprintf(f, "---- console tty%d ----\n%s\n", atomic_load(&g_active), snap); fclose(f); }
            }
        }
    }
}

static void vt_present_active(void) {
    if (!vt_enabled()) return;
    int act = atomic_load(&g_active);
    if (act >= 1 && act <= VT_MAX_TEXT) vt_render_and_present(&g_vt[act - 1].con);
}

// Reader thread: drain the six pty masters into their consoles and re-present on change / blink.
static int g_autoswitch;      // RDVT_AUTOSWITCH=<1..6>: switch to this console ~2s after boot (test seam)
static void* vt_reader(void* arg) {
    (void)arg;
    int ticks = 0, autodone = 0;
    double last_blink = 0.0;
    // Opt-in test input: a private FIFO lets an isolated boot test type through
    // the same pty master as physical keyboard input. Never enabled by default.
    int test_input = -1;
    const char* input_path = getenv("RDVT_TEST_INPUT_FIFO");
    if (input_path && *input_path) {
        struct stat before, after;
        if (lstat(input_path, &before) == 0 && S_ISFIFO(before.st_mode) &&
            before.st_uid == getuid() && (before.st_mode & 077) == 0) {
            test_input = open(input_path, O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
            if (test_input >= 0 && (fstat(test_input, &after) != 0 ||
                after.st_dev != before.st_dev || after.st_ino != before.st_ino)) {
                close(test_input); test_input = -1;
            }
        }
    }
    if (getenv("RDVT_TRACE"))
        fprintf(stderr, "[vt] reader started pid=%d masters=%d,%d,%d,%d,%d,%d\n",
                (int)getpid(), g_vt[0].master, g_vt[1].master, g_vt[2].master,
                g_vt[3].master, g_vt[4].master, g_vt[5].master);
    for (;;) {
        struct pollfd fds[VT_MAX_TEXT + 1];
        for (int i = 0; i < VT_MAX_TEXT; i++) {
            fds[i].fd = g_vt[i].master;
            fds[i].events = POLLIN;
            fds[i].revents = 0;
        }
        fds[VT_MAX_TEXT] = (struct pollfd){ .fd = test_input, .events = POLLIN };
        int pr = poll(fds, VT_MAX_TEXT + (test_input >= 0), 200);
        int dirty = 0, got = 0;
        if (pr > 0) {
            if (test_input >= 0 && (fds[VT_MAX_TEXT].revents & POLLIN)) {
                char keys[256];
                ssize_t n = read(test_input, keys, sizeof keys);
                if (n > 0) vt_feed_key(keys, (size_t)n);
            }
            for (int i = 0; i < VT_MAX_TEXT; i++) {
                if (!(fds[i].revents & POLLIN)) continue;
                uint8_t buf[4096];
                ssize_t n;
                while ((n = read(g_vt[i].master, buf, sizeof buf)) > 0) {
                    if (getenv("RDVT_TRACE"))
                        fprintf(stderr, "[vt] reader pid=%d tty%d read %zd bytes\n",
                                (int)getpid(), i + 1, n);
                    if (getenv("RDCONSOLE_LOG")) {
                        static FILE* cf;
                        if (!cf) cf = fopen("/tmp/rd_console.log", "a");
                        if (cf) { fwrite(buf, 1, (size_t)n, cf); fflush(cf); }
                    }
                    console_put_bytes(&g_vt[i].con, buf, (size_t)n);
                    dirty = 1;
                    got = 1;
                }
            }
        }
        // Cursor blink on wall-clock time, not loop iterations: a master whose slave is closed
        // (HUP) makes poll() return immediately, and an iteration counter would blink + re-render
        // the whole 1280x720 surface in a tight spin at 100% CPU.
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            double now = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
            if (last_blink == 0.0 || now - last_blink >= 400.0) {
                last_blink = now;
                for (int i = 0; i < VT_MAX_TEXT; i++) console_blink_tick(&g_vt[i].con);
                dirty = 1;
            }
        }
        if (g_autoswitch && !autodone && ++ticks == 10) {   // ~2s: exercise vt_switch_to()
            autodone = 1;
            vt_switch_to(g_autoswitch);
        }
        if (dirty) vt_present_active();
        // HUP/ERR with no data is level-triggered; without a throttle poll() returns immediately
        // and the loop busy-spins. A short sleep keeps the reader idle between real events.
        if (pr > 0 && !got) usleep(50000);
    }
    return NULL;
}

static int g_cols, g_rows;

static void vt_read_env(void) {
    g_cols = 160; g_rows = 45;       // 1280x720 @ 8x16
    const char* c = getenv("RDCONS_COLS");
    const char* r = getenv("RDCONS_ROWS");
    if (c && *c) { int v = atoi(c); if (v >= 20 && v <= CON_MAX_COLS) g_cols = v; }
    if (r && *r) { int v = atoi(r); if (v >= 5 && v <= CON_MAX_ROWS) g_rows = v; }
    g_fb_w = g_cols * 8;
    g_fb_h = g_rows * 16;
    const char* d = getenv("RDCONS_DUMP");
    g_dump = d && *d && strcmp(d, "0") != 0;
    const char* a = getenv("RDVT_AUTOSWITCH");
    if (a && *a) { int v = atoi(a); if (v >= 1 && v <= VT_MAX_TEXT) g_autoswitch = v; }
}

// True when this shim process owns the console renderer + reader thread: the process the user
// launched and every re-exec of it (shebang, `exec` in a script — both keep the PID). Forked
// gettys get a new PID and are not renderers.
static int vt_is_renderer(void) {
    const char* rp = getenv("RDVT_ROOT_PID");
    if (!rp || !*rp) return 1;
    return atoi(rp) == (int)getpid();
}

void vt_init(void) {
    if (!vt_enabled()) return;
    pthread_mutex_lock(&g_lock);
    if (g_inited) { pthread_mutex_unlock(&g_lock); return; }
    g_inited = 1;

    vt_read_env();
    int renderer = vt_is_renderer();
    if (getenv("RDVT_TRACE"))
        fprintf(stderr, "[vt] init pid=%d rootpid=%s renderer=%d\n",
                (int)getpid(), getenv("RDVT_ROOT_PID") ? getenv("RDVT_ROOT_PID") : "?", renderer);

    // Explicit per-boot identity, not a scan of low fds: reserved masters live above
    // 256 and executors close ordinary descriptors before opening StandardInput=tty.
    // Only the renderer holds masters; exec children reopen its existing slave nodes.
    int saved_fd[VT_MAX_TEXT];
    char saved_slave[VT_MAX_TEXT][128];
    unsigned long long saved_dev[VT_MAX_TEXT], saved_ino[VT_MAX_TEXT];
    int inherited = 0;
    const char* state = getenv("RDVT_STATE");
    if (state) {
        const char* p = state;
        for (int i = 0; i < VT_MAX_TEXT; i++) {
            int used = 0;
            if (sscanf(p, "%d,%llu,%llu,%127[^;];%n", &saved_fd[i], &saved_dev[i],
                       &saved_ino[i], saved_slave[i], &used) != 4 ||
                used == 0 || saved_fd[i] < 256 || strncmp(saved_slave[i], "/dev/ttys", 9)) break;
            p += used;
            inherited++;
        }
        if (*p) inherited = 0;
    }
    char exported[1024] = {0};
    size_t exported_len = 0;
    for (int i = 0; i < VT_MAX_TEXT; i++) {
        vt_unit* u = &g_vt[i];
        u->master = -1;
        if (inherited == VT_MAX_TEXT) {
            snprintf(u->slave, sizeof u->slave, "%s", saved_slave[i]);
            char actual[128] = {0};
            int valid = ioctl(saved_fd[i], TIOCPTYGNAME, actual) == 0 &&
                        !strcmp(actual, saved_slave[i]);
            if (renderer) {
                if (!valid) {
                    fprintf(stderr, "[vt] renderer lost master for tty%d\n", i + 1);
                    u->slave[0] = 0;
                    continue;
                }
                u->master = saved_fd[i];
            } else if (valid) {
                // Do not leak the renderer's high fds into guest activation fdsets.
                close(saved_fd[i]);
            }
        } else if (!renderer) {
            // A missing boot identity must not silently create six unrelated consoles.
            fprintf(stderr, "[vt] missing renderer identity for tty%d\n", i + 1);
            continue;
        } else if (vt_make_pty(&u->master, u->slave, sizeof u->slave) != 0) {
            fprintf(stderr, "[vt] tty%d pty creation FAILED (%s)\n", i + 1, strerror(errno));
            u->master = -1;
            continue;
        }
        if (!u->slave[0]) continue;
        struct stat st;
        // stat() the slave device node for its dev/ino (vt_owns_fd needs them) WITHOUT opening it.
        // An open+close here would CONSUME the TIOCPTYUNLK unlock (Darwin re-locks a BSD pty slave
        // after its first open is closed) and leave the guest's later open() blocked forever.
        if (stat(u->slave, &st) != 0 ||
            (inherited == VT_MAX_TEXT && ((unsigned long long)st.st_dev != saved_dev[i] ||
                                         (unsigned long long)st.st_ino != saved_ino[i]))) {
            fprintf(stderr, "[vt] missing or recycled tty%d slave\n", i + 1);
            u->slave[0] = 0;
            continue;
        }
        u->slave_dev = st.st_dev; u->slave_ino = st.st_ino;
        // Report the console geometry through TIOCGWINSZ so getty formats to it.
        struct winsize ws;
        ws.ws_row = (unsigned short)g_rows;
        ws.ws_col = (unsigned short)g_cols;
        ws.ws_xpixel = (unsigned short)g_fb_w;
        ws.ws_ypixel = (unsigned short)g_fb_h;
        if (renderer) {
            ioctl(u->master, TIOCSWINSZ, &ws);
            int fl = fcntl(u->master, F_GETFL, 0);
            fcntl(u->master, F_SETFL, fl | O_NONBLOCK);
            u->master = shim_fd_reserve(u->master);
            // Preserve a renderer re-exec (same PID); children close these during vt_init.
            fcntl(u->master, F_SETFD, 0);
            exported_len += (size_t)snprintf(exported + exported_len,
                sizeof exported - exported_len, "%d,%llu,%llu,%s;", u->master,
                (unsigned long long)u->slave_dev, (unsigned long long)u->slave_ino, u->slave);
        }
        fprintf(stderr, "[vt] tty%d %s master=%d slave=%s dev=%llu ino=%llu (%dx%d)\n",
                i + 1, inherited == VT_MAX_TEXT ? "reusing inherited" : "ready",
                u->master, u->slave,
                (unsigned long long)u->slave_dev, (unsigned long long)u->slave_ino,
                g_cols, g_rows);
    }

    if (renderer) setenv("RDVT_STATE", exported, 1);

    // Only the root process renders: it allocates the framebuffer, keeps the console grids, and
    // runs the reader thread that drains the masters into pixels. Forked gettys just service
    // open/read/write/ioctl on the shared slaves.
    if (renderer) {
        g_fb = (uint8_t*)malloc((size_t)g_fb_w * (size_t)g_fb_h * 4);
        if (g_fb) memset(g_fb, 0, (size_t)g_fb_w * (size_t)g_fb_h * 4);
        for (int i = 0; i < VT_MAX_TEXT; i++) console_init(&g_vt[i].con, g_cols, g_rows);
        pthread_t th;
        int prc = pthread_create(&th, NULL, vt_reader, NULL);
        if (getenv("RDVT_TRACE"))
            fprintf(stderr, "[vt] renderer pid=%d pthread_create=%d fb=%p\n",
                    (int)getpid(), prc, (void*)g_fb);
        if (prc == 0) pthread_detach(th);
    }
    pthread_mutex_unlock(&g_lock);
}

int vt_is_node(const char* gpath) {
    if (!gpath) return 0;
    if (strncmp(gpath, "/dev/tty", 8) == 0) {
        const char* p = gpath + 8;
        if (p[0] >= '1' && p[0] <= '6' && p[1] == 0) return 1;
    }
    return (strcmp(gpath, "/dev/console") == 0) || (strcmp(gpath, "/dev/tty0") == 0);
}

int vt_node_number(const char* gpath) {
    if (gpath && strncmp(gpath, "/dev/tty", 8) == 0) {
        const char* p = gpath + 8;
        if (p[0] >= '1' && p[0] <= '6' && p[1] == 0) return p[0] - '0';
    }
    return 0;
}

int vt_open_node(const char* gpath) {
    vt_init();
    int n = vt_node_number(gpath);
    if (n == 0) n = atomic_load(&g_active);          // /dev/console, /dev/tty0 -> current
    if (n < 1 || n > VT_MAX_TEXT) n = 1;
    vt_unit* u = &g_vt[n - 1];
    if (!u->slave[0]) return -LX_ENOTTY;
    // Darwin re-locks a pty slave after the FIRST open+close (vt_init opens it once just to read
    // its dev/ino). Re-unlock before every guest open or the open() blocks waiting for a lock that
    // will never arrive — systemd's open(/dev/console) hung here with no error, stalling boot.
    if (u->master >= 0) ioctl(u->master, TIOCPTYUNLK);
    int fd = open(u->slave, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -(long)linux_errno_from_host(errno);
    struct stat st;
    // Never attach a stale service to a tty recycled after its renderer exited.
    if (fstat(fd, &st) != 0 || st.st_dev != u->slave_dev || st.st_ino != u->slave_ino) {
        close(fd); return -LX_ENOTTY;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0) {
        int err = errno; close(fd); return -(long)linux_errno_from_host(err);
    }
    atomic_fetch_add(&g_vt_open[n - 1], 1);
    return fd;
}

int vt_number_for_fd(int fd) {
    if (!vt_enabled() || fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0) return 0;
    for (int i = 0; i < VT_MAX_TEXT; i++)
        if (g_vt[i].slave_dev == st.st_dev && g_vt[i].slave_ino == st.st_ino) return i + 1;
    return 0;
}

int vt_owns_fd(int fd) { return vt_number_for_fd(fd) != 0; }

// Guest close(2) of a console fd drops that console's open count. The count is a best-effort
// mirror of "a guest process has this tty open"; a dup'd fd still dev/ino-matches here, so the
// counter can briefly read 0 while another dup stays open — harmless, because logind's autovt
// start is idempotent (systemd no-ops an already-running getty@ttyN).
void vt_fd_closed(int fd) {
    int n = vt_number_for_fd(fd);
    if (n < 1 || n > VT_MAX_TEXT) return;
    int expect = atomic_load(&g_vt_open[n - 1]);
    while (expect > 0 && !atomic_compare_exchange_weak(&g_vt_open[n - 1], &expect, expect - 1)) {}
}

void vt_stat_identity(const char* gpath, uint64_t* dev, uint64_t* ino) {
    int n = vt_node_number(gpath);
    if (n == 0) n = atomic_load(&g_active);
    if (n < 1 || n > VT_MAX_TEXT) n = 1;
    *dev = (uint64_t)g_vt[n - 1].slave_dev;
    *ino = (uint64_t)g_vt[n - 1].slave_ino;
}

void vt_fake_stat(unsigned int* mode, uint64_t* rdev, const char* gpath) {
    int n = vt_node_number(gpath);
    // /dev/tty0 is the current-console alias (4:0); /dev/console and /dev/ttyN resolve to a
    // concrete console (4:N). Keep minor 0 for tty0 so it matches Linux's makedev(4, 0).
    if (n == 0 && gpath && strcmp(gpath, "/dev/tty0") != 0) n = 1;
    *mode = 0620u | 0020000u;               // S_IFCHR, mode 0620 (tty owner rw, group w)
    *rdev = ((uint64_t)4 << 8) | (uint64_t)n;  // Linux console: major 4, minor 0 or N
}

long vt_ioctl(int fd, unsigned long req, unsigned long arg) {
    (void)fd;
    switch (req) {
    case LX_KDGKBMODE: *(int*)arg = LX_K_XLATE; return 0;
    case LX_KDGKBTYPE: *(int*)arg = LX_KB_101; return 0;
    case LX_KDGKBLED:  *(int*)arg = 0; return 0;
    case LX_KDGETLED:  *(int*)arg = 0; return 0;
    case LX_KIOCSOUND: case LX_KDMKTONE: return 0;               // beep: no-op
    case LX_KDSKBMODE: case LX_KDSETMODE: case LX_KDGETMODE:
    case LX_KDSKBMETA: case LX_KDSKBLED:
        return 0;
    case LX_VT_OPENQRY: *(int*)arg = 1; return 0;
    case LX_VT_GETMODE: {
        struct lx_vt_mode* m = (struct lx_vt_mode*)arg;
        m->mode = LX_VT_AUTO; m->waitv = 0; m->relsig = 0; m->acqsig = 0; m->frsig = 0;
        return 0;
    }
    case LX_VT_SETMODE: return 0;
    case LX_VT_GETSTATE: {
        struct lx_vt_stat* s = (struct lx_vt_stat*)arg;
        s->v_active = (unsigned short)vt_active();
        s->v_signal = 0;
        // Linux: bit 0 is /dev/tty0 (always open); bit N (N>=1) is ttyN "in use" (vt_in_use():
        // the vc's tty port kref > 1, i.e. a process has it open). logind's vt_is_busy() reads
        // exactly this mask from /dev/tty1 and SKIPS spawning autovt@ttyN (getty@ttyN) when bit N
        // is set. Reporting all six as allocated would mark tty2..6 busy and suppress auto-VT, so
        // report only consoles a guest has actually opened (g_vt_open). Under-reporting is harmless:
        // manager_start_unit() of an already-running getty@ttyN is a no-op.
        unsigned short state = 1;                                    // /dev/tty0
        for (int i = 0; i < VT_MAX_TEXT; i++)
            if (atomic_load(&g_vt_open[i]) > 0)
                state = (unsigned short)(state | (1u << (i + 1)));
        s->v_state = state;
        return 0;
    }
    case LX_VT_ACTIVATE:    vt_switch_to((int)arg); return 0;
    case LX_VT_WAITACTIVE: {
        // Linux vt_waitactive(): return immediately if the VT is already active, otherwise sleep
        // until a VT switch makes it active (loop to tolerate a switch to some other VT first), and
        // report -EINTR if the wait was interrupted. arg is the VT number directly (like VT_ACTIVATE,
        // a bare _IO), so it is read as an int, not a guest pointer. The GUI console (7) blocks like
        // a text VT; anything outside 1..7 is refused before it can crash the state machine.
        int n = (int)arg;
        if (n < 1 || n > VT_GRAPHICS) return -LX_EINVAL;
        pthread_mutex_lock(&g_waitactive_lock);
        while (atomic_load(&g_active) != n) {
            int rc = pthread_cond_wait(&g_waitactive_cond, &g_waitactive_lock);
            if (rc != 0) {
                pthread_mutex_unlock(&g_waitactive_lock);
                return rc == EINTR ? -LX_EINTR : -(long)linux_errno_from_host(rc);
            }
        }
        pthread_mutex_unlock(&g_waitactive_lock);
        return 0;
    }
    case LX_VT_RELDISP:     return 0;
    case LX_VT_DISALLOCATE: return 0;
    case LX_VT_RESIZE:      return 0;
    case LX_VT_RESIZEX:     return 0;
    case LX_GIO_FONT: case LX_PIO_FONT: case LX_GIO_FONTX: case LX_PIO_FONTX:
        return 0;                                             // we own the font
    default:
        return -LX_ENOTTY;
    }
}
