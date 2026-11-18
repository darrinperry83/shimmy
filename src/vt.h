// vt.h — Linux virtual consoles (/dev/tty1..tty6) for the shim.
//
// Each VT is backed by a real Darwin pty pair so the *line discipline* (termios canonical mode,
// echo, ISIG signal generation) comes from the host kernel for free: the guest's /dev/ttyN is the
// pty SLAVE, and the shim holds the MASTER. What the guest writes to the slave appears on the
// master and is rendered by a text console; what the shim writes to the master is what the guest
// reads. This is the same split a Linux kernel VT driver has between the tty layer (line
// discipline) and the console layer (rendering), just drawn along the pty boundary instead of
// inside the kernel.
#ifndef RDRIFT_VT_H
#define RDRIFT_VT_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VT_MAX_TEXT 6          // tty1..tty6
#define VT_GRAPHICS 7          // the "GUI" console (Hyprland/Wayland)

// Present hook: the GUI build registers gpuwin_present_fmt here; CLI leaves it NULL (no-op).
typedef long (*vt_present_fn)(const uint8_t* rgba, uint64_t w, uint64_t h, int bgra);

int  vt_enabled(void);                    // RDVT=0 disables the whole subsystem
void vt_init(void);                       // idempotent: create ptys + consoles + reader thread
void vt_set_present_fn(vt_present_fn fn);

// Device-node queries, used by the openat/stat paths in syscalls.c.
int  vt_is_node(const char* gpath);       // /dev/ttyN (1..6), /dev/console, /dev/tty0
int  vt_node_number(const char* gpath);   // 1..6 for /dev/ttyN, else 0
int  vt_open_node(const char* gpath);     // -> fresh slave fd (host fd) or negative errno
int  vt_owns_fd(int fd);                  // is fd (or any dup of it) one of our pty slaves?
int  vt_number_for_fd(int fd);            // guest tty number for a pty slave, or 0
void vt_fd_closed(int fd);                // guest closed a console fd: drop its open count
int  vt_console_active_open(void);        // -> fresh read-end fd for /sys/class/tty/tty0/active
int  vt_console_active_owns_fd(int fd);   // is fd (or a dup) the active-console pipe read end?
int  vt_claim_ctty(int fd);
int  vt_get_foreground(int fd, int* pgid);
int  vt_set_foreground(int fd, int pgid);
int  vt_get_session(int fd, int* sid);
void vt_stat_identity(const char* gpath, uint64_t* dev, uint64_t* ino);
void vt_fake_stat(unsigned int* mode, uint64_t* rdev, const char* gpath);

long vt_ioctl(int fd, unsigned long req, unsigned long arg);  // returns Linux errno on failure

// Host-keyboard integration (window.m).
void vt_switch_to(int n);                 // n = 1..6 text console, 7 = GUI
int  vt_active(void);                     // current console number (1..7)
int  vt_text_input_active(void);          // keyboard should go to the console, not the GUI
void vt_feed_key(const char* utf8, size_t n);  // feed bytes into the active VT's pty master
void vt_graphics_frame(void);             // a desktop frame was presented -> switch to GUI console
void vt_graphics_session_lost(void);      // producer exited -> clear old frame, allow next greeter

#ifdef __cplusplus
}
#endif
#endif
