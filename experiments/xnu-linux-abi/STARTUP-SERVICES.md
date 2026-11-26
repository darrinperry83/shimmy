# Preserve Apple startup while replacing display and login

The target keeps the supporting Apple services and replaces the WindowServer
and login UI implementations. A minimal init that discards the rest of the
startup environment is not the intended final architecture. The P0 VM test's
disabled jobs establish only a kernel syscall checkpoint, not a complete
replacement display or service environment.

The existing bin/shimmy takeover implementation already copies the complete
WindowServer job and changes its executable and one environment variable.
The generated build/com.apple.WindowServer.plist files preserve the job's
MachServices, POSIXSpawnType and _Conclave. This is useful existing work;
copying the definition does not establish replacement protocol compatibility
or prove that a duplicate-label override takes precedence on a target boot.

## Read-only inventory

Run startup_inventory.py against a reference system or mounted root:

```sh
python3 experiments/xnu-linux-abi/startup_inventory.py
```

The output is out/startup/inventory.json. It records complete definitions,
source paths and SHA-256 digests, including non-job configuration plists.
It retains all other jobs and marks the two display/login labels as needing
verified replacement providers. Duplicate labels are reported separately;
no effective precedence or startup order is inferred. The initial host scan
includes both a System and Library WindowServer definition. No installed
service is changed by the inventory tool.

## WindowServer job obligations

The inspected stock 25F80 job declares these Mach services:

- com.apple.CARenderServer
- com.apple.CoreDisplay.Notification
- com.apple.CoreDisplay.master
- com.apple.SpatialConnect.host
- com.apple.VirtualDisplay
- com.apple.backboard.hid-services.xpc
- com.apple.iohideventsystem
- com.apple.windowserver
- com.apple.windowserver.active
- com.apple.windowserver.watchdog

Preserve the exact nested dictionaries, including HideUntilCheckIn,
ResetAtClose and _ExperimentalNonLaunching. These are endpoints associated
with one job, not a list of ten separately launched processes. Each provider
must check in and implement its required protocols; declaring a name or
returning dummy success is insufficient. Map them against the existing
WindowServer replacement before enabling dependent clients.

Also retain the job's _Conclave association with
com.apple.backboardd.conclave. Its lifecycle and purpose require validation;
it must not be discarded or treated as an ordinary standalone LaunchDaemon
without evidence. The stock loginwindow job separately declares
com.apple.loginwindow and has MultipleInstances and private RunningBoard
properties. Its non-UI session duties need auditing before replacement.

## Acceptance milestones

1. Keep the normal Apple service environment and inventory actual loaded jobs.
2. Map each display/login endpoint and companion to existing replacement code,
   required external services, and missing implementation.
3. Prove target launchd loads the intended replacement definition, rather than
   relying on duplicate-label precedence or service-name races.
4. Exercise real broker/display/HID clients while Apple WindowServer and
   loginwindow executables are absent, and record service health and errors.
5. Validate the selected services on the physical hardware separately from VM
   behavior, including actual sleep, wake, display and input operation.

The existing replacement source is referenced by repository documents as
shim-metal-clear/src/windowserver.c; that file is not present in this checkout.
The rdmetal and scanout submodules are currently uninitialized. Their source
and the replacement server must be inspected before claiming full coverage.
