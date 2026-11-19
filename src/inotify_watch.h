#ifndef SHIMMY_INOTIFY_WATCH_H
#define SHIMMY_INOTIFY_WATCH_H

// Watch identity and bounded kqueue-backed notifications for watched files.
// Directory child-name events and full Linux inotify lifecycle remain unsupported.
// Caller owns the kqueue fd and must keep it out of guest fd cleanup.
int inotify_watch_register(int read_fd, int write_fd, int kq_fd,
                           int (*reserve_fd)(int), void (*unreserve_fd)(int));
long inotify_watch_add(int fd, const char *host_path, unsigned mask);
// Accept a watch on a path with no host backing store (a shim-synthesized path such as a child
// cgroup dir). Returns a real watch descriptor that never fires — an honest no-op matching the
// shim's "no enforced limits" cgroup design. The path is only recorded for identity.
long inotify_watch_add_noop(int fd, const char *logical_path, unsigned mask);
long inotify_watch_remove(int fd, int wd);
// Track guest-visible aliases; explicit close of the final alias tears down
// the worker, watches and private descriptors. Return 1 if close was handled.
int inotify_watch_can_dup(int old_fd, int new_fd);
void inotify_watch_dup(int old_fd, int new_fd);
int inotify_watch_is_fd(int fd);
int inotify_watch_close(int fd);

#endif
