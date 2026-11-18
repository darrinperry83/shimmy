# ARM64 `linkat` (#37) flags

The existing pathname handler accepted unknown flag bits and could create a
hard link for a request Linux rejects. It now checks flags before looking up
either pathname or changing the host filesystem. The probe also found that
Darwin `link()` followed a symlink source for the ordinary zero-flag call;
the handler now uses `linkat()` so zero links the symlink inode and
`AT_SYMLINK_FOLLOW` links its target. `AT_EMPTY_PATH` is a
valid Linux flag but needs a source-file-descriptor path that this handler
does not model, so it returns `EOPNOTSUPP` instead of linking the wrong file.

The disposable ARM64 probe checks unknown flags with valid and invalid source
addresses, the unsupported empty-path flag, ordinary symlink linking, and
source-following behavior, including source versus symlink inode identity. It
checks final files on the host side. Guest-root
path resolution, symlink races, permissions and cross-filesystem behavior are
not established by this one probe.

The isolated branch also passed `./build.sh cli` and a private Arch boot using
the current task-3 fixture: a fresh login prompt appeared and D-Bus broker and
launcher settled. The harness stopped its exact guest process and the cloned
rootfs was removed. The fixture copy warned about protected entries, so this
remains a boot smoke test, not full rootfs fidelity.

```sh
./build.sh cli
python3 tests/run-linkat-flags.py
```
