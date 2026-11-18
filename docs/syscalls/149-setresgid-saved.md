# ARM64 #149 `setresgid`: saved-ID request in host-identity mode

Darwin `setregid` cannot express Linux's third, saved-GID argument. Previously
Shimmy ignored it, potentially returning success without installing the
requested saved GID. In ordinary host-identity mode an explicit saved-GID
request now returns `EOPNOTSUPP` before any real or effective host credential
change. `(-1, -1, -1)` remains a no-op. The `RDUID0` virtual credential path
continues to model all three IDs independently.

This is a bounded safeguard, not full Linux credentials. Requests specifying
the current saved GID explicitly are also refused in ordinary mode. Existing
real/effective changes with saved GID `-1` still depend on Darwin semantics.

Proof: `./build.sh cli`; `python3 tests/run-setresgid-saved.py` compiles a
disposable ARM64 guest and verifies refusal, unchanged IDs, and the no-op
without changing host credentials. The existing `run-getresgid.py` covers the
separate virtual-root saved-GID model. A private Arch boot clone reached a
fresh login prompt with the D-Bus broker and launcher settled; that smoke test
does not establish complete credential or desktop compatibility.
