// drmasahi.h — a DRM render node for Mesa's Asahi driver. See drmasahi.c for why.
#ifndef DRMASAHI_H
#define DRMASAHI_H
#include <stdint.h>
#include <stddef.h>

int  drm_enabled(void);                 // present by default; RDDRM=0 hides
int  drm_is_node(const char* path);     // is this one of the nodes we present?
void drm_register_fd(int fd);
void drm_forget_fd(int fd);
int  drm_owns_fd(int fd);
long drm_ioctl(int fd, unsigned long req, unsigned long arg);
void* drm_mmap_bo(int fd, long off);      // GEM_MMAP_OFFSET fake offset -> backing, NULL if none
void* drm_bo_map_by_fd(int fd, size_t* size);  // PRIME_HANDLE_TO_FD carrier fd -> BO backing
void drm_fake_stat(unsigned int* mode, unsigned long* rdev, const char* path);
int  drm_sysfs_kind(const char* path);         // 0 none | 1 char | 2 uevent | 3 dir | 4 link | 5 pci-uevent | 6 pci-id
const char* drm_sysfs_uevent(void);            // content for the synthetic /sys/.../uevent
const char* drm_sysfs_link(void);              // target for /sys/.../device/subsystem
const char* drm_sysfs_pci_content(const char* path); // body for /sys/.../device/{uevent,vendor,...}

#endif
