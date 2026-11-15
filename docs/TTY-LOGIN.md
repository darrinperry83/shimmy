# Shimmy TTY login

`./run.sh --init` boots Arch systemd. TTY1 through TTY6 each start a normal
`getty@ttyN` login; the desktop is on TTY7. Use Ctrl+Option+F1–F6 to switch to
a text TTY and Ctrl+Option+F7 to return to the desktop. Some Mac keyboards
also require the Fn key for F1–F7.

An Arch base root has no account for the macOS user. From a macOS terminal,
run `./setup-login.sh` in the Shimmy checkout before booting. It asks for the
Linux username and password locally, creates a normal account and home
directory, and sets its password through the guest's `chpasswd`. No password
is embedded in the repository or chosen by Shimmy. `ROOTFS=/path/to/rootfs
./setup-login.sh` selects a different installed root.

The TTY login starts a shell. Desktop startup through a display manager or a
user's login session is a separate milestone; a login prompt alone does not
establish that a desktop session works.
