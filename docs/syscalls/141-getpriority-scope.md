# ARM64 syscall 141: `getpriority` scope

The old handler sent guest process, group and user IDs directly to Darwin,
which could disclose priorities of unrelated macOS processes. Linux resolves
those selectors within its own [process namespace](https://github.com/torvalds/linux/blob/v6.12/kernel/sys.c#L280-L345).

This bounded path queries only the current host Shimmy process when the guest
requests `PRIO_PROCESS` with `who=0` or its current guest PID. It returns the
raw Linux ABI value `20 - nice`. An unknown positive PID returns `ESRCH`; a
known child and group/user selectors return `EOPNOTSUPP` until a safe guest
model exists. Invalid selectors return `EINVAL`. It does not query another
host PID, group, or UID. Host niceness and guest scheduling may still differ.

Verification: `./build.sh cli` and `python3 tests/run-getpriority-scope.py`.
The probe checks self and guest-PID aliases plus safe rejection of impossible
positive IDs. The host sandbox permits the self priority read.
