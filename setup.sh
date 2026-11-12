#!/usr/bin/env bash
# shimmy — one-time host setup + scanout injection.
#   1. checks SIP is disabled
#   2. verifies the passwordless-sudo rule for the injector
#   3. injects the scanout payload for the current macOS version
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INJECTOR="$HERE/scanout/ws_inject_manual_d2"
PAYLOAD="$HERE/scanout/payload/rdgpu-scanout-server18-payload"

echo "==> 1/3 SIP status"
if csrutil status 2>/dev/null | grep -qi 'enabled'; then
  echo "    ! SIP is ENABLED. The injection needs it disabled:"
  echo "      reboot into Recovery -> Terminal -> 'csrutil disable' -> reboot."
  exit 1
fi
echo "    SIP disabled (or status unavailable) — ok."

echo "==> 2/3 passwordless sudo for the injector"
if ! sudo -n "$INJECTOR" --version >/dev/null 2>&1 && ! sudo -n true >/dev/null 2>&1; then
  echo "    ! no NOPASSWD sudo. Add to /etc/sudoers (sudo visudo):"
  echo "      $(whoami) ALL=(ALL) NOPASSWD: $INJECTOR"
fi
echo "    sudo available — ok."

echo "==> 3/3 inject the scanout payload"
if [ ! -x "$INJECTOR" ]; then echo "    ! injector not found: $INJECTOR"; exit 1; fi
if [ ! -f "$PAYLOAD" ]; then
  echo "    ! payload not found: $PAYLOAD (build the scanout submodule first)"
  exit 1
fi
sudo -n "$INJECTOR" --register "$PAYLOAD"
echo "    done. Scanout is live until WindowServer next restarts."
