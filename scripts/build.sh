#!/usr/bin/env bash
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
      -DCMAKE_LINKER=/usr/bin/ld.lld-18 \
      -DWITH_SSL=OFF \
      -DFIREBOLT_ADBC_BUILD_TESTS=ON \
      -G Ninja -S . -B build
    cmake --build build -j"$(nproc)"
  '
