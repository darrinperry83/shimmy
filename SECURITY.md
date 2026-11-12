# Security Policy

## Reporting a vulnerability

Do **not** open a public issue for a security vulnerability. Report it privately to the
maintainers. We will acknowledge within 48 hours and work with you on a fix and disclosure.

## Scope

- The shim's syscall translation and Mach/Wayland surfaces.
- The scanout injection (it disables SIP and runs code inside WindowServer — treat it as
  privileged).

Note: disabling SIP for the scanout is a deliberate trade-off and is called out in the README.
