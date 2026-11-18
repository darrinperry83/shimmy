# Native #85 timerfd_create slot reuse

`timerfd_create_shim` used its slot-array high-water mark as the live count.
Once 32 slots had ever been allocated, it returned `EMFILE` even when all
timers were closed. Native #57 `close` also returned success early for a
reserved timerfd read end, leaving its timer source and slot live forever.

The bounded correction scans for a closed slot before applying the 32-live-
timer limit. Ordinary guest `close` retires only a recognized timerfd read
end, releases its reservation and closes that descriptor. A vfork child still
cannot close the parent's shared timerfd or any other shim-private descriptor.

`tests/run-timerfd-create-reuse.py` checks invalid clock and flags, 32 live
timers, the 33rd `EMFILE`, replacement after close, and 40 more create/close
cycles in a disposable native ARM64 guest. Existing gettime and settime guest
probes remain regression checks. This does not establish full timerfd fork,
duplication, clock-jump cancellation, or close-race semantics. The Arch
login/D-Bus gate is a boot smoke test, not proof of those cases.
