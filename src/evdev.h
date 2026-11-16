// evdev.h — a synthetic /dev/input/event* node pair (keyboard + pointer) for the
// native-DRM input path. Hyprland's libinput (via aquamarine's udev backend) opens
// /dev/input/event0 and /dev/input/event1, issues EVIOCG* ioctls, and reads struct
// input_event records. window.m drains its NSEvent queue (the same source that used to
// feed wlsrv's wl_seat) and calls evdev_inject* to push those events here.
#ifndef RDMETAL_EVDEV_H
#define RDMETAL_EVDEV_H
#include <stdint.h>

#define EVDEV_DEV_KBD  0       // /dev/input/event0 — keyboard (EV_KEY)
#define EVDEV_DEV_PTR  1       // /dev/input/event1 — pointer  (EV_KEY + EV_REL)

// linux/input-event-codes.h values the pump emits (kept here so window.m and evdev.c
// share one source of truth; evdev.c re-derives the ioctl side from these).
#define EVDEV_EV_KEY    0x01
#define EVDEV_EV_REL    0x02
#define EVDEV_REL_X     0x00
#define EVDEV_REL_Y     0x01
#define EVDEV_REL_HWHEEL 0x06
#define EVDEV_REL_WHEEL 0x08

int  evdev_is_node(const char* path);       // /dev/input/event0|event1
int  evdev_open_node(const char* path);     // new fd (dup of the device's event-pipe read end)
void evdev_register_fd(int fd, int dev);
void evdev_forget_fd(int fd);
int  evdev_owns_fd(int fd);
int  evdev_device_of_fd(int fd);            // EVDEV_DEV_* or -1
long evdev_ioctl(int fd, unsigned long req, unsigned long arg);
void evdev_fake_stat(unsigned int* mode, unsigned long* rdev, const char* path);

// sysfs / udev surface for libinput's udev backend (same shape as drmasahi.c/snd_alsa.c).
int  evdev_sysfs_kind(const char* path);    // 0 none, else a kind code
const char* evdev_sysfs_content(const char* path); // body for uevent/dev/udev-db files (NULL if none)
const char* evdev_sysfs_link(void);         // target for /sys/class/input/<name>/subsystem

// Event injection (called from window.m's input pump). Each logical event must be
// followed by evdev_syn(dev). Events are dropped (never block) if the node is not open
// or its pipe is full.
void evdev_inject(int dev, uint16_t type, uint16_t code, int32_t value);
void evdev_syn(int dev);

// In native systemd boot the GUI owner and the compositor are separate host
// processes. Start the per-boot, nonblocking input sender in the GUI owner;
// evdev_open_node registers the compositor's receiver automatically.
int evdev_shared_host_init(void);

#endif
