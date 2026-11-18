#ifndef SHIMMY_INOTIFY_WATCH_H
#define SHIMMY_INOTIFY_WATCH_H

// Watch identity and IN_IGNORED delivery for Shimmy's currently quiet inotify pipe.
// This does not synthesize filesystem-change events.
int inotify_watch_register(int read_fd, int write_fd);
long inotify_watch_add(int fd, const char *host_path, unsigned mask);
long inotify_watch_remove(int fd, int wd);

#endif
