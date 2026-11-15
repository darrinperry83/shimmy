#!/bin/bash
# Build the isolated diagnostic CLI from the same vendored sources as development.
set -euo pipefail
cd "$(dirname "$0")"
./build.sh cli
cp shim_cli shim_cli_login_candidate
cp build/shim_cli.srcsha build/shim_cli_login_candidate.srcsha
echo "   -> ./shim_cli_login_candidate"
