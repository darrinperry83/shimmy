#!/usr/bin/env python3
"""Probe host filesystem confinement around one Shimmy CLI process.

This is a negative test, not a Landlock implementation or production launcher.
Run outside a pre-existing macOS sandbox, with an existing read-only test root:

    python3 tests/test_host_filesystem_sandbox.py --root /path/to/test-root

An Arch root with curl can additionally use ``--network-url URL`` to check
network reachability under the same filesystem policy.

Only this script's disposable build/ files are written. The supplied root is
read but not granted host filesystem write access by the test profile.
"""

import argparse
import fcntl
import os
from pathlib import Path
import resource
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile


def quote_profile_path(path: Path) -> str:
    # Seatbelt's profile language uses quoted strings, not shell quoting.
    return str(path).replace("\\", "\\\\").replace('"', '\\"')


def no_core() -> None:
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def make_profile(binary: Path, root: Path, group: Path,
                 network: bool = False) -> str:
    # This is a *test* profile. Package-manager completeness is unproven;
    # even a passing file probe must not be used as a pacman fallback.
    profile = (
        '(version 1)(deny default)(allow process-exec)(allow process-fork)'
        '(allow file-read-metadata (subpath "/"))'
        '(allow file-read* (literal "/"))'
        '(allow file-read* (subpath "/System"))'
        '(allow file-read* (subpath "/usr/lib"))'
        '(allow file-read* (subpath "/Library"))'
        '(allow file-read* (subpath "/private/var/db"))'
        '(allow file-read* (subpath "/dev"))'
        '(allow file-read* (literal "/private/tmp"))'
        f'(allow file-read* (subpath "{quote_profile_path(group)}"))'
        f'(allow file-write* (subpath "{quote_profile_path(group)}"))'
        '(allow file-write* (literal "/dev/ptmx"))'
        '(allow file-write* (literal "/dev/null"))'
        f'(allow file-read* (literal "{quote_profile_path(binary)}"))'
        f'(allow file-read* (subpath "{quote_profile_path(root)}"))'
    )
    if network:
        # DNS and HTTP are intentionally broad for this diagnostic probe.
        profile += (f'(allow network-bind (subpath "{quote_profile_path(group)}"))'
                    '(allow network-outbound)'
                    '(allow mach-lookup (global-name "com.apple.system.opendirectoryd.libinfo"))')
    return profile


def run_guest(binary: Path, root: Path, args: list[str], profile: str | None,
              socket_group: Path,
              pass_fds: tuple[int, ...] = (),
              clean_initial: bool = False,
              stdin_data: str | None = None) -> tuple[int, str]:
    command = [str(binary), str(root), *args]
    if profile is not None:
        command = ["/usr/bin/sandbox-exec", "-p", profile, *command]
    # Keep host credentials and unrelated session settings out of the guest.
    env = {"PATH": "/usr/bin:/bin", "HOME": "/root",
           "RDSOCKETGROUP": str(socket_group)}
    if clean_initial:
        env["RDHOST_FD_CLEAN"] = "1"
    proc = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            stdin=subprocess.PIPE if stdin_data is not None else None,
                            start_new_session=True, preexec_fn=no_core,
                            close_fds=True, pass_fds=pass_fds)
    try:
        output, _ = proc.communicate(input=stdin_data, timeout=20)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        output, _ = proc.communicate()
        raise RuntimeError(f"guest timed out: {args!r}; last output: {output[-500:]}")
    return proc.returncode, output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--shim", default="./shim_cli", type=Path)
    parser.add_argument("--network-url", help="optional URL for a guest curl HEAD probe")
    args = parser.parse_args()
    binary = args.shim.resolve(strict=True)
    root = args.root.resolve(strict=True)
    if not (root / "etc/os-release").is_file():
        parser.error("test root needs /etc/os-release")
    if not (root / "bin/sh").exists() and not (root / "bin/sh").is_symlink():
        parser.error("test root needs /bin/sh")

    build = Path(__file__).resolve().parents[1] / "build"
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="host-sandbox-probe-", dir=build) as temp:
        directory = Path(temp).resolve()
        secret = directory / "read-secret"
        write_target = directory / "write-target"
        secret.write_text("SHIMMY_TEST_SECRET\n")
        write_target.write_text("unchanged\n")
        group = Path("/private/tmp") / f"shimmy-sandbox-test-{os.getpid()}"
        profile = make_profile(binary, root, group, bool(args.network_url))
        try:
            baseline_rc, baseline = run_guest(binary, root,
                ["/bin/cat", str(secret)], None, group)
            if baseline_rc != 0 or "SHIMMY_TEST_SECRET" not in baseline:
                raise AssertionError("baseline guest cannot read the host-shared probe")

            root_rc, root_output = run_guest(binary, root,
                ["/bin/cat", "/etc/os-release"], profile, group)
            if root_rc != 0 or "ID=" not in root_output:
                raise AssertionError("confined guest cannot read its Linux root")

            denied_rc, denied_output = run_guest(binary, root,
                ["/bin/cat", str(secret)], profile, group)
            if denied_rc == 0 or "SHIMMY_TEST_SECRET" in denied_output:
                raise AssertionError("confined guest read the denied host file")

            child_ok_rc, child_ok_output = run_guest(binary, root,
                ["/bin/sh", "-c", "cat /etc/os-release"], profile, group)
            if child_ok_rc != 0 or "ID=" not in child_ok_output:
                raise AssertionError("guest child could not read an allowed Linux file")

            child_rc, child_output = run_guest(binary, root,
                ["/bin/sh", "-c", "cat " + shlex.quote(str(secret))],
                profile, group)
            if child_rc == 0 or "SHIMMY_TEST_SECRET" in child_output:
                raise AssertionError("guest child read the denied host file")

            grandchild_rc, grandchild_output = run_guest(binary, root,
                ["/bin/sh", "-c", "sh -c " +
                 shlex.quote("cat " + shlex.quote(str(secret)))], profile, group)
            if grandchild_rc == 0 or "SHIMMY_TEST_SECRET" in grandchild_output:
                raise AssertionError("guest grandchild read the denied host file")

            write_rc, _ = run_guest(binary, root,
                ["/bin/sh", "-c", "printf attempted > " +
                 shlex.quote(str(write_target))], profile, group)
            if write_rc == 0 or write_target.read_text() != "unchanged\n":
                raise AssertionError("confined guest wrote the denied host file")

            # A path sandbox cannot revoke a descriptor opened before it was
            # applied. Prove both that our normal launcher closes it and that
            # deliberately passing it through would expose the contents.
            inherited_fd = os.open(secret, os.O_RDONLY)
            try:
                if inherited_fd > 9:
                    raise RuntimeError("test needs a single-digit inherited fd")
                fd_command = ["/bin/sh", "-c", f"cat <&{inherited_fd}"]
                _, closed_output = run_guest(binary, root, fd_command,
                                             profile, group)
                if "SHIMMY_TEST_SECRET" in closed_output:
                    raise AssertionError("normal launcher leaked a host descriptor")
                passed_rc, passed_output = run_guest(binary, root, fd_command,
                    profile, group, pass_fds=(inherited_fd,))
                if passed_rc != 0 or "SHIMMY_TEST_SECRET" not in passed_output:
                    raise AssertionError("pre-opened descriptor risk probe changed")
                cleaned_rc, cleaned_output = run_guest(binary, root, fd_command,
                    profile, group, pass_fds=(inherited_fd,), clean_initial=True)
                if cleaned_rc == 0 or "SHIMMY_TEST_SECRET" in cleaned_output:
                    raise AssertionError("initial Shimmy descriptor sanitizer leaked host fd")
            finally:
                os.close(inherited_fd)

            # The host scan must include descriptors above the usual 0..255
            # range and non-file capabilities such as connected Unix sockets.
            base_fd = os.open(secret, os.O_RDONLY)
            high_fd = fcntl.fcntl(base_fd, fcntl.F_DUPFD, 300)
            try:
                high_path = f"/proc/self/fd/{high_fd}"
                high_args = ["/bin/cat", high_path]
                high_open_rc, high_open_out = run_guest(binary, root, high_args,
                    profile, group, pass_fds=(high_fd,))
                if high_open_rc != 0 or "SHIMMY_TEST_SECRET" not in high_open_out:
                    raise AssertionError("high numbered file descriptor probe did not reach guest")
                high_clean_rc, high_clean_out = run_guest(binary, root, high_args,
                    profile, group, pass_fds=(high_fd,), clean_initial=True)
                if high_clean_rc == 0 or "SHIMMY_TEST_SECRET" in high_clean_out:
                    raise AssertionError("sanitizer leaked high numbered host file descriptor")
                high_child_rc, high_child_out = run_guest(binary, root,
                    ["/bin/sh", "-c", "sh -c " +
                     shlex.quote("cat " + high_path)], profile, group,
                    pass_fds=(high_fd,), clean_initial=True)
                if high_child_rc == 0 or "SHIMMY_TEST_SECRET" in high_child_out:
                    raise AssertionError("descendant recovered sanitized host descriptor")
            finally:
                os.close(high_fd)
                os.close(base_fd)

            socket_result = "socket-fd-untested-no-bash"
            if (root / "bin/bash").exists():
                sock_a, sock_b = socket.socketpair()
                socket_fd = fcntl.fcntl(sock_a.fileno(), fcntl.F_DUPFD, 512)
                try:
                    sock_b.sendall(b"SHIMMY_SOCKET_SECRET\n")
                    socket_args = ["/bin/bash", "-c",
                        f'IFS= read -r -u {socket_fd} x || exit 91; printf "%s\\n" "$x"']
                    sock_open_rc, sock_open_out = run_guest(binary, root, socket_args,
                        profile, group, pass_fds=(socket_fd,))
                    if sock_open_rc != 0 or "SHIMMY_SOCKET_SECRET" not in sock_open_out:
                        raise AssertionError("inherited Unix socket probe did not reach guest")
                    sock_clean_rc, sock_clean_out = run_guest(binary, root, socket_args,
                        profile, group, pass_fds=(socket_fd,), clean_initial=True)
                    if sock_clean_rc == 0 or "SHIMMY_SOCKET_SECRET" in sock_clean_out:
                        raise AssertionError("sanitizer leaked inherited Unix socket")
                    socket_result = "socket-fd-closed"
                finally:
                    os.close(socket_fd)
                    sock_a.close()
                    sock_b.close()

            blocked_scan = profile + '(deny file-read* (subpath "/dev/fd"))'
            scan_rc, scan_out = run_guest(binary, root,
                ["/bin/cat", "/etc/os-release"], blocked_scan, group,
                clean_initial=True)
            if scan_rc == 0 or "cannot sanitize inherited host descriptors" not in scan_out:
                raise AssertionError("sanitizer did not fail closed on fd enumeration error")

            # Sanitization is first-entry only: a descriptor that the guest
            # opens itself must survive its ordinary fork/exec path.
            guest_fd_rc, guest_fd_output = run_guest(binary, root,
                ["/bin/sh", "-c", "exec 9</etc/os-release; sh -c 'cat <&9'"],
                profile, group, clean_initial=True)
            if guest_fd_rc != 0 or "ID=" not in guest_fd_output:
                raise AssertionError("sanitizer broke legitimate guest fd inheritance")
            stdin_rc, stdin_output = run_guest(binary, root, ["/bin/cat"],
                profile, group, clean_initial=True, stdin_data="GUEST_STDIN_OK\n")
            if stdin_rc != 0 or "GUEST_STDIN_OK" not in stdin_output:
                raise AssertionError("sanitizer broke standard input or output")

            if args.network_url:
                curl_rc, curl_output = run_guest(binary, root,
                    ["/usr/bin/curl", "--head", "--location", "--fail",
                     "--max-time", "15", "--silent", "--show-error",
                     args.network_url], profile, group)
                if curl_rc != 0:
                    raise AssertionError("confined guest curl failed: " + curl_output[-300:])

            result = ("PASS root-read host-read-denied child-read-denied "
                      "grandchild-read-denied host-write-denied launcher-fd-closed "
                      "shim-initial-fd-closed high-fd-closed high-fd-child-closed "
                      + socket_result + " "
                      "scan-failure-closed guest-fd-exec-preserved stdio-preserved "
                      "KNOWN_LIMITATION unsanitized-preopened-fd-bypasses-path-policy")
            if args.network_url:
                result += " guest-network"
            print(result)
            return 0
        finally:
            shutil.rmtree(group, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, RuntimeError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        sys.exit(1)
