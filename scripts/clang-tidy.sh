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

# Run clang-tidy (checks in .clang-tidy) over the driver and unit-test sources,
# inside the same firebolt-adbc-builder image scripts/build.sh uses, so every
# machine runs the same clang-tidy-18 against the same compile_commands.json.
#
#   ./scripts/clang-tidy.sh                  # every .cpp under src/ and tests/unit/
#   ./scripts/clang-tidy.sh src/HttpClient.cpp ...   # just these (the pre-commit hook)
#
# Needs a build/ produced by scripts/build.sh: clang-tidy reads its
# compile_commands.json, and generated headers (Version.h) live there.
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

IMAGE="firebolt-adbc-builder:latest"

if [ ! -f build/compile_commands.json ]; then
  echo "build/compile_commands.json is missing; run ./scripts/build.sh first." >&2
  exit 1
fi
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "$IMAGE is missing; run ./scripts/build.sh first." >&2
  exit 1
fi
if ! docker run --rm "$IMAGE" which run-clang-tidy-18 >/dev/null; then
  echo "$IMAGE predates clang-tidy; rebuild it: docker rmi $IMAGE && ./scripts/build.sh" >&2
  exit 1
fi

# Headers are checked through the sources that include them, so only .cpp
# files are passed on; an edited header alone means checking everything.
files=()
for f in "$@"; do
  case "$f" in
    *.cpp) files+=("$REPO_ROOT/$f") ;;
    *) files=(); break ;;
  esac
done
if [ ${#files[@]} -eq 0 ]; then
  files=("$REPO_ROOT/(src|tests/unit)/.*\.cpp")
fi

docker run --rm \
  -u "$(id -u):$(id -g)" \
  -v "$REPO_ROOT:$REPO_ROOT" \
  -w "$REPO_ROOT" \
  "$IMAGE" \
  run-clang-tidy-18 -p build -quiet -j"$(nproc)" \
    -header-filter="^$REPO_ROOT/(src|tests/unit)/" \
    "${files[@]}"
