#!/usr/bin/env bash
# Run the C++ unit tests via ctest. Assumes scripts/build.sh has run.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)/build"
ctest --output-on-failure
