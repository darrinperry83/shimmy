# `move_pages` (native ARM64 syscall 239): bounded query safety

The existing one-node query path read the guest page-address array and wrote
the status array directly after checking only that the addresses were mapped
in the host process. A host mapping is not proof that memory belongs to the
guest. The Linux [query path](https://github.com/torvalds/linux/blob/master/mm/migrate.c)
uses checked user accesses and reports page results through the status array.

This change stages at most 256 page addresses and result codes in host memory,
copies the addresses through Shimmy's guest-memory registry, and copies the
completed results back through checked guest memory. It also requires each
queried page to be guest mapped before using the host residency query. The
existing one-node, current-process query behavior is retained. Larger arrays
return `EOPNOTSUPP`; page migration, remote process queries, and NUMA placement
remain unsupported.

`python3 tests/run-move-pages-query.py` compiles a freestanding ARM64 guest
to query one resident anonymous page and one invalid page, and checks invalid
array pointers, unsupported size/migration, and invalid flags. `./build.sh
cli`, the existing move-pages regression, and the new guest probe pass in the
isolated worktree. A private Arch boot using the current `task-3` fixture also
reached a fresh login prompt with the D-Bus broker and launcher settled. Its
scoped guest and exact clone were cleaned afterward.
