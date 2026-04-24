#!/usr/bin/env bash
set -euo pipefail

IMAGE="${IMAGE:-231290928314.dkr.ecr.us-east-1.amazonaws.com/packdb-builder:1.0.8}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -f "${SCRIPT_DIR}/CMakeLists.txt" && -f "${SCRIPT_DIR}/adbc.h" ]]; then
  # Script is in adbc directory (expected).
  ADBC_DIR="${SCRIPT_DIR}"
elif [[ -d "${SCRIPT_DIR}/adbc" ]]; then
  # Fallback if script is copied to packdb root.
  ADBC_DIR="${SCRIPT_DIR}/adbc"
else
  echo "error: could not resolve adbc directory from script location: ${SCRIPT_DIR}" >&2
  exit 1
fi

BUILD_DIR="${ADBC_DIR}/build_in_docker"
mkdir -p "${BUILD_DIR}"

docker run --rm \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -v "${ADBC_DIR}:/workspace/adbc" \
  -w /workspace/adbc \
  "${IMAGE}" \
  bash -lc '
    set -euo pipefail

    if [[ ! -d submodule ]]; then
      echo "error: adbc/submodule is missing; run: git submodule update --init --recursive adbc/submodule" >&2
      exit 1
    fi

    cmake -S /workspace/adbc -B /workspace/adbc/build_in_docker \
      -G Ninja \
      -DCMAKE_C_COMPILER=clang-18 \
      -DCMAKE_CXX_COMPILER=clang++-18 \
      -DCMAKE_BUILD_TYPE=Release \
      -DFIREBOLT_ADBC_BUILD_TESTS=ON

    cmake --build /workspace/adbc/build_in_docker -j"$(nproc)"
    ctest --test-dir /workspace/adbc/build_in_docker --output-on-failure
  '

echo "Build and test completed successfully."
echo "Artifacts are in: ${BUILD_DIR}"
