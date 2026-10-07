#!/usr/bin/env bash
# Copyright (c) 2026 ADBC Drivers Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail

if [[ $# -ne 2 ]]; then
  printf 'usage: %s <linux|macos|windows> <amd64|arm64>\n' "$0" >&2
  exit 2
fi

platform="$1"
arch="$2"

case "$arch" in
  amd64 | arm64)
    ;;
  *)
    printf 'unsupported architecture: %s\n' "$arch" >&2
    exit 2
    ;;
esac

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

build_dir="${repo_root}/build/ci-test-${platform}-${arch}"

ctest=(ctest)
if command -v pixi >/dev/null 2>&1; then
  ctest=(pixi exec -s cmake ctest)
fi

"${ctest[@]}" --test-dir "$build_dir" --build-config Debug --output-on-failure

if [[ "$platform" == "linux" && "$arch" == "amd64" ]]; then
  FIREBOLT_ADBC_BUILD_DIR="$build_dir" "${repo_root}/scripts/clang-tidy.sh"
fi

# The examples are user-facing documentation. Exercise their imports and CLI
# setup once in CI without requiring credentials or a running Firebolt engine.
if [[ "$platform" == "linux" && "$arch" == "amd64" ]]; then
  python=(python3)
  if command -v pixi >/dev/null 2>&1; then
    python=(pixi run python)
  fi

  for example in "${repo_root}"/examples/python/*.py; do
    printf 'Checking %s --help\n' "${example#"${repo_root}/"}"
    "${python[@]}" "$example" --help >/dev/null
  done
fi
