# The earlier ELF/PTY checks must pass before the independent kernel core starts.
echo LINUX_CORE_TEST_STARTING
export_logs
# Export independently: a fault in the tested kernel must not lose its last
# successful callback. This monitor is finite and only runs in the test VM.
(
    for tick in $(jot 300); do
        sysctl -n kern.rdxnu_linux_core_log > /var/tmp/rdxnu-core-live.log 2>/dev/null || true
        cp /var/tmp/rdxnu-core-live.log '/Volumes/My Shared Files/results/core-live.log' || true
        sleep 0.1
    done
) &
sysctl -w kern.rdxnu_linux_core=1
completed=no
for attempt in $(jot 60); do
    status=$(sysctl -n kern.rdxnu_linux_core)
    if [ "$status" = 2 ]; then completed=yes; break; fi
    if [ "$status" -lt 0 ]; then break; fi
    sleep 1
done
dmesg | tail -250
[ "$completed" = yes ] || { echo LINUX_CORE_TEST_FAILED_OR_TIMED_OUT; exit 1; }
echo LINUX_CORE_KERNEL_STATUS_PASS
export_logs
