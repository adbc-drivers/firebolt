#!/usr/bin/env bash
# Run the pytest-based integration suite against a 1-node Firebolt Core.
#
# Forwards extra args to runner.py, e.g.
#   scripts/test-integration.sh --core-image=...:latest -k test_connect
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
exec "$REPO_ROOT/tests/integration/runner.py" "$@"
