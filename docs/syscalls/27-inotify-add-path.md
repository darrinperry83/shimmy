# ARM64 #27 inotify_add_watch guest path

The #27 handler snapshots the bounded guest pathname with checked copy-in
before path resolution or watch registration. An invalid guest address now
returns `-EFAULT` without crashing the shim or consuming a watch ID.
Existing mask handling, including `IN_DONT_FOLLOW`, is unchanged.

`python3 tests/run-inotify-add-watch-path.py` uses a disposable root and
checks a malformed pointer, an executable static pathname, watch ID
allocation, removal and descriptor close. Existing inotify guest tests cover
event delivery separately. This change does not establish full Linux inotify
coalescing, symlink, move, overflow or inherited-worker behavior.
