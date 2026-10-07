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

# Run clang-tidy against the compile database produced by the generated Linux
# test build. GitHub's Ubuntu runner provides clang-tidy 18. A checkout-only
# pre-commit run has no compile database and skips this hook; generated build CI
# invokes it after compilation.
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

build_dir="${FIREBOLT_ADBC_BUILD_DIR:-build/ci-test-linux-amd64}"
if [[ ! -f "$build_dir/compile_commands.json" ]]; then
  printf 'Skipping clang-tidy: %s/compile_commands.json is missing\n' "$build_dir"
  exit 0
fi

runner="${RUN_CLANG_TIDY:-run-clang-tidy-18}"
if ! command -v "$runner" >/dev/null 2>&1; then
  printf '%s is missing; install clang-tidy 18 or set RUN_CLANG_TIDY\n' "$runner" >&2
  exit 1
fi

files=()
for file in "$@"; do
  case "$file" in
    *.cpp) files+=("$repo_root/$file") ;;
    *) files=(); break ;;
  esac
done
if [[ ${#files[@]} -eq 0 ]]; then
  files=("$repo_root/(src|tests/unit)/.*\\.cpp")
fi

"$runner" -p "$build_dir" -quiet \
  -header-filter="^$repo_root/(src|tests/unit)/" \
  "${files[@]}"
