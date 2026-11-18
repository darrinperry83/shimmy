# ARM64 `truncate` (#45)

The extended dispatcher previously passed a raw guest pathname to its
resolver and then to Darwin `truncate`. It now snapshots a bounded guest
pathname before host filesystem work and rejects a negative length.

The disposable ARM64 probe checks a bad guest path (`EFAULT`), a negative
length (`EINVAL`), an unterminated `PATH_MAX` path (`ENAMETOOLONG`), a missing
file (`ENOENT`), and actual shortening and zero extension of a file inside
the disposable root. The CLI build and probe pass. A private Arch boot reached
a fresh login prompt with the D-Bus broker and launcher settled. The harness
stopped its scoped guest and the exact clone was removed. Protected-path
permission warnings during fixture copy make this a boot smoke test.

The existing path resolver and host filesystem translation remain unchanged.
This change does not establish general pathname or filesystem parity.

```sh
./build.sh cli
python3 tests/run-truncate.py
```
