# Official emptty greeter probe (2026-11-16)

This was a bounded experiment in a disposable copy of the Arch Linux ARM root. It did not change the live root, display, package policy, or Shimmy source.

- The official `extra` repository lists `emptty 0.16.1-1` for aarch64 (about 1.1 MB download). The fetched package's SHA-256 matched `extra.db`, and `pacman-key --verify` accepted its detached signature with full trust from the Arch Linux ARM Build System. `pacman -U` installed it in the private root. One post-transaction hook warned, but the binary, PAM service, and systemd unit were present.
- In that root only, `display-manager.service` pointed to `emptty.service`; its configured TTY was changed from 7 to 6 because Shimmy currently exposes six text VTs. The existing tty6 getty was disabled in the clone.
- With `RDPIDFD_SOCKET=1 RDSOCKETOUTQ_COMPAT=1`, a full isolated systemd boot reached `graphical.target` and kept `/usr/bin/emptty -d` running. The process had the tty6 slave open as fd 3. Shimmy switched to tty6, but no greeter text appeared. A Return typed through the private VT test FIFO produced only a two-byte terminal echo. No PAM login or Wayland session was reached.
- The persistent journal recorded `Started emptty display manager` but no emptty error. A one-second host sample found the guest Go worker waiting through Shimmy futex calls, with another guest thread in epoll. This does not by itself identify whether it was waiting for TTY input, logind, or another event.

Therefore, official package availability and service startup are verified; an interactive greeter is **not**. The next useful probe would trace emptty's guest syscalls just before its first terminal write, or run its own debug logging if supported. The isolated root and temporary packages/logs were removed after the experiment.
