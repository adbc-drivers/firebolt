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

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

license_files=(
  "LICENSE.txt"
  "license.tpl"
  "submodule/nanoarrow/LICENSE.txt"
  "submodule/boringssl/LICENSE"
  "submodule/boringssl/third_party/fiat/LICENSE"
  "submodule/curl/COPYING"
  "submodule/json/LICENSE.MIT"
)

for license_file in "${license_files[@]}"; do
  if [[ ! -f "${repo_root}/${license_file}" ]]; then
    printf 'required license file is missing: %s\n' "$license_file" >&2
    exit 1
  fi
done

append_component() {
  local component="$1"
  shift

  printf '\n%s:\n\n' "$component"
  for license_file in "$@"; do
    cat "${repo_root}/${license_file}"
    printf '\n'
  done
}

cat "${repo_root}/LICENSE.txt"
printf '\n'
cat "${repo_root}/license.tpl"

append_component \
  "Apache Arrow nanoarrow" \
  "submodule/nanoarrow/LICENSE.txt"
append_component "BoringSSL" "submodule/boringssl/LICENSE"
append_component \
  "fiat-crypto (included by BoringSSL)" \
  "submodule/boringssl/third_party/fiat/LICENSE"
append_component "curl" "submodule/curl/COPYING"
append_component "nlohmann/json" "submodule/json/LICENSE.MIT"
