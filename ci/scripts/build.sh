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

if [[ $# -ne 3 ]]; then
  printf 'usage: %s <test|release> <linux|macos|windows> <amd64|arm64>\n' "$0" >&2
  exit 2
fi

mode="$1"
platform="$2"
arch="$3"

case "$mode" in
  test)
    export CMAKE_BUILD_TYPE="Debug"
    ;;
  release)
    export CMAKE_BUILD_TYPE="Release"
    ;;
  *)
    printf 'unsupported build mode: %s\n' "$mode" >&2
    exit 2
    ;;
esac

case "$platform" in
  linux)
    library_ext="so"
    ;;
  macos)
    library_ext="dylib"
    ;;
  windows)
    library_ext="dll"
    ;;
  *)
    printf 'unsupported platform: %s\n' "$platform" >&2
    exit 2
    ;;
esac

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
build_dir="${repo_root}/build/ci-${mode}-${platform}-${arch}"
output_library="${repo_root}/build/libadbc_driver_firebolt.${library_ext}"

cmake=(cmake)
generator_args=()
if command -v pixi >/dev/null 2>&1; then
  cmake=(pixi exec -s cmake -s ninja cmake)
  if [[ "$platform" != "windows" ]]; then
    generator_args=(-G Ninja)
  fi
elif [[ "$platform" != "windows" ]] && command -v ninja >/dev/null 2>&1; then
  generator_args=(-G Ninja)
fi

configure_args=(
  -S "$repo_root"
  -B "$build_dir"
  -DFIREBOLT_ADBC_BUILD_TESTS=ON
  -DWITH_SSL=ON
)
if [[ "$platform" == "linux" ]]; then
  configure_args+=(
    -DCMAKE_C_COMPILER=gcc
    -DCMAKE_CXX_COMPILER=g++
  )
elif [[ "$platform" == "windows" ]]; then
  configure_args+=(
    -DFIREBOLT_ADBC_STRIP_DEP_DEBUG=OFF
  )
fi

build_args=(--build "$build_dir" --config "$CMAKE_BUILD_TYPE" --parallel)

case "${CMAKE_VERBOSE:-}" in
  1 | ON | TRUE | true | yes | YES)
    set -x
    configure_args+=(--log-level=VERBOSE -DCMAKE_VERBOSE_MAKEFILE=ON)
    build_args+=(--verbose)
    ;;
esac

"${cmake[@]}" "${configure_args[@]}" "${generator_args[@]}"
"${cmake[@]}" "${build_args[@]}"

built_library="$(
  find "$build_dir" \
    \( -type f -o -type l \) \
    \( -name "libadbc_driver_firebolt.${library_ext}" -o -name "adbc_driver_firebolt.${library_ext}" \) \
    -print \
    -quit
)"
if [[ -z "$built_library" ]]; then
  printf 'could not find built Firebolt driver in %s\n' "$build_dir" >&2
  exit 1
fi

cp -L "$built_library" "$output_library"
chmod 755 "$output_library"
printf 'Built %s\n' "$output_library"
