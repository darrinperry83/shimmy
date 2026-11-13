// v4l2cam.h — a V4L2 capture node backed by the Mac's camera. See v4l2cam.c.
#ifndef V4L2CAM_H
#define V4L2CAM_H
#include <stdint.h>
int  cam_enabled(void);
int  cam_is_node(const char* path);
void cam_register_fd(int fd);
void cam_forget_fd(int fd);
int  cam_owns_fd(int fd);
void cam_fake_stat(unsigned int* mode, unsigned long* rdev);
long cam_ioctl(int fd, unsigned long req, unsigned long arg);
#endif
