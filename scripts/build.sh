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

# Configure + build libfirebolt_adbc.so and the unit-test executable inside
# the pinned firebolt-adbc-builder Docker image (Ubuntu 22.04 + clang-18).  The
# 22.04 glibc gives the resulting .so broader runtime compatibility.
#
# On first run the script builds the image from docker/builder/Dockerfile;
# subsequent runs reuse the cached image.  Delete the image (`docker rmi
# firebolt-adbc-builder:latest`) to force a rebuild.
#
# Idempotent and safe to run from any CWD within this repository.
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

IMAGE="firebolt-adbc-builder:latest"
DOCKERFILE_DIR="$REPO_ROOT/docker/builder"

if docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "$IMAGE already present locally; skipping rebuild."
else
  echo "Building $IMAGE from $DOCKERFILE_DIR/Dockerfile ..."
  docker build -t "$IMAGE" "$DOCKERFILE_DIR"
fi

git submodule update --init --recursive

docker run --rm \
  -u "$(id -u):$(id -g)" \
  -v "$REPO_ROOT:$REPO_ROOT" \
  -w "$REPO_ROOT" \
  "$IMAGE" \
  bash -c '
    set -euo pipefail
    cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=/usr/bin/clang-18 \
      -DCMAKE_CXX_COMPILER=/usr/bin/clang++-18 \
      -DWITH_SSL=ON \
      -DFIREBOLT_ADBC_BUILD_TESTS=ON \
      -G Ninja -S . -B build
    cmake --build build -j"$(nproc)"
  '
