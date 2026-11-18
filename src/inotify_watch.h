#ifndef SHIMMY_INOTIFY_WATCH_H
#define SHIMMY_INOTIFY_WATCH_H

// Watch identity and bounded kqueue-backed notifications for watched files.
// Directory child-name events and full Linux inotify lifecycle remain unsupported.
// Caller owns the kqueue fd and must keep it out of guest fd cleanup.
int inotify_watch_register(int read_fd, int write_fd, int kq_fd,
                           int (*reserve_fd)(int), void (*unreserve_fd)(int));
long inotify_watch_add(int fd, const char *host_path, unsigned mask);
long inotify_watch_remove(int fd, int wd);

#endif
