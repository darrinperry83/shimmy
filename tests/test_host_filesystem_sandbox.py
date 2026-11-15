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
import os
from pathlib import Path
import resource
import shlex
import shutil
import signal
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
              pass_fds: tuple[int, ...] = ()) -> tuple[int, str]:
    command = [str(binary), str(root), *args]
    if profile is not None:
        command = ["/usr/bin/sandbox-exec", "-p", profile, *command]
    # Keep host credentials and unrelated session settings out of the guest.
    env = {"PATH": "/usr/bin:/bin", "HOME": "/root",
           "RDSOCKETGROUP": str(socket_group)}
    proc = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            start_new_session=True, preexec_fn=no_core,
                            close_fds=True, pass_fds=pass_fds)
    try:
        output, _ = proc.communicate(timeout=20)
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
            finally:
                os.close(inherited_fd)

            if args.network_url:
                curl_rc, curl_output = run_guest(binary, root,
                    ["/usr/bin/curl", "--head", "--location", "--fail",
                     "--max-time", "15", "--silent", "--show-error",
                     args.network_url], profile, group)
                if curl_rc != 0:
                    raise AssertionError("confined guest curl failed: " + curl_output[-300:])

            result = ("PASS root-read host-read-denied child-read-denied "
                      "grandchild-read-denied host-write-denied launcher-fd-closed "
                      "KNOWN_LIMITATION preopened-fd-bypasses-path-policy")
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
