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

# ADBC's package generator derives the package version from the Git tag, while
# the compiled driver reports the CMake project version. Reject a release whose
# tag would therefore disagree with the binary and its SONAME.
if [[ "$mode" != "release" || "${GITHUB_REF_TYPE:-}" != "tag" ]]; then
  exit 0
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

cmake_version="$(
  sed -n \
    's/^[[:space:]]*project(adbc_driver_firebolt VERSION \([0-9][0-9.]*\).*/\1/p' \
    "${repo_root}/CMakeLists.txt"
)"
if [[ -z "$cmake_version" ]]; then
  printf 'could not parse the project version from CMakeLists.txt\n' >&2
  exit 1
fi

tag_version="${GITHUB_REF_NAME#v}"
printf 'CMake project version: %s\n' "$cmake_version"
printf 'Git tag version: %s\n' "$tag_version"

if [[ "$tag_version" != "$cmake_version" ]]; then
  printf \
    'tag %s does not match project(adbc_driver_firebolt VERSION %s)\n' \
    "$GITHUB_REF_NAME" \
    "$cmake_version" >&2
  exit 1
fi
