# Guest keyring subscription model

`src/guest_key_subscription.c` adds an internal, explicit subscription model
over the private keyring store. It searches thread, then process, then session
rings. A ring must belong to the subscription's guest UID before it can be
bound. The fork-child model drops thread and process subscriptions and keeps
the session; the exec model does the same. These transitions match the Linux
kernel's [key retention documentation](https://docs.kernel.org/security/keys/core.html).

The focused ARM64 test checks search precedence, a different guest UID being
denied, the fork/exec transition model, and a separate re-exec process finding
the session key through the shared namespace. The previous shared-store test
still passes. Both tests use disposable synthetic payloads.

**No syscall is enabled.** `add_key`, `request_key`, and `keyctl` remain
`ENOSYS`. The test passes a session serial as a trusted test argument; Shimmy
does not yet perform an authenticated subscription handoff across its actual
guest fork and re-exec paths. Nor does it create default user/session rings,
enforce Linux permission masks or possession, expire keys, or provide an
upcall. The `guest_uid` argument to the internal constructor must eventually
come from Shimmy's authoritative guest credential state, never guest-supplied
memory. A linked key owned by another UID is still inaccessible in this
prototype, whereas Linux can grant possession and other permissions.

The prototype store also remains plaintext and in-place without transaction
recovery. It must not receive real guest key material or be exposed through
the ABI until storage confidentiality, crash behavior, and the above
authorization and lifecycle gaps are resolved and tested in a private ARM64
guest boot. A native test of the model is a dependency gate, not a Linux
compatibility claim.

Focused test:

```sh
clang -arch arm64 -O2 -Wall -Wextra -Werror -Isrc \
  tests/guest-key-subscription.c src/guest_key_subscription.c \
  src/guest_key_shared.c -o /tmp/shimmy-guest-key-subscription-test
/tmp/shimmy-guest-key-subscription-test
```
