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

# On GitHub Actions each phase gets a collapsible log group and a row in the
# job summary.
github_actions=false
if [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then
  github_actions=true
fi

summarize() {
  local phase="$1"
  local outcome="$2"
  if [[ "$github_actions" != "true" || -z "${GITHUB_STEP_SUMMARY:-}" ]]; then
    return 0
  fi
  if [[ ! -s "$GITHUB_STEP_SUMMARY" ]]; then
    printf '| Phase | Outcome |\n| --- | --- |\n' >>"$GITHUB_STEP_SUMMARY"
  fi
  printf '| %s | %s |\n' "$phase" "$outcome" >>"$GITHUB_STEP_SUMMARY"
}

# Runs one phase and returns its status. The command runs in an `||` context,
# where `set -e` does not apply, so a multi-command phase must return its own
# failures.
run_phase() {
  local phase="$1"
  shift
  if [[ "$github_actions" == "true" ]]; then
    printf '::group::%s\n' "$phase"
  fi
  local status=0
  "$@" || status=$?
  if [[ "$github_actions" == "true" ]]; then
    printf '::endgroup::\n'
  fi
  if [[ $status -eq 0 ]]; then
    summarize "$phase" "passed"
  else
    summarize "$phase" "failed"
  fi
  return "$status"
}

ctest=(ctest)
python=(python3)
if command -v pixi >/dev/null 2>&1; then
  ctest=(pixi exec -s cmake ctest)
  python=(pixi run python)
fi

run_phase "Unit tests" \
  "${ctest[@]}" --test-dir "$build_dir" --build-config Debug --output-on-failure

# Everything below needs Docker or Linux-only tools, which the CI runners have
# on Linux only.
if [[ "$platform" != "linux" || "$arch" != "amd64" ]]; then
  exit 0
fi

# A pull request's checkout is the merge of the PR into its base (HEAD^1), so
# only the .cpp files it changes need clang-tidy. A change to a header, the
# checks, the build or a dependency can affect any file, so it checks every
# one, as does every other event.
clang_tidy_scope="all"
clang_tidy_files=()
if [[ "${GITHUB_EVENT_NAME:-}" == "pull_request" ]]; then
  changed_files="$(git -C "$repo_root" diff --name-only --diff-filter=d HEAD^1 HEAD)"
  clang_tidy_scope="changed"
  while IFS= read -r changed_file; do
    case "$changed_file" in
      *.cpp)
        clang_tidy_files+=("$changed_file")
        ;;
      *.h | *.h.in | .clang-tidy | CMakeLists.txt | scripts/clang-tidy.sh | submodule/*)
        clang_tidy_scope="all"
        clang_tidy_files=()
        break
        ;;
    esac
  done <<<"$changed_files"
fi

export FIREBOLT_ADBC_BUILD_DIR="$build_dir"
if [[ "$clang_tidy_scope" == "all" ]]; then
  run_phase "clang-tidy (all files)" "${repo_root}/scripts/clang-tidy.sh"
elif [[ ${#clang_tidy_files[@]} -gt 0 ]]; then
  run_phase "clang-tidy (${#clang_tidy_files[@]} changed files)" \
    "${repo_root}/scripts/clang-tidy.sh" "${clang_tidy_files[@]}"
else
  printf 'Skipping clang-tidy: the pull request changes no C++ files\n'
  summarize "clang-tidy" "skipped: no C++ changes"
fi

# The examples are user-facing documentation. Exercise their imports and CLI
# setup without requiring credentials or a running Firebolt engine.
check_examples() {
  local example
  for example in "${repo_root}"/examples/python/*.py; do
    printf 'Checking %s --help\n' "${example#"${repo_root}/"}"
    "${python[@]}" "$example" --help >/dev/null || return 1
  done
}
run_phase "Examples" check_examples

# The live FB2 suite runs separately below: it needs secrets and does not block.
run_phase "Integration tests" \
  "${repo_root}/tests/integration/runner.py" --ignore=tests/fb2_legacy_live

# FB2 only: tests/fb2_legacy_live against a real Firebolt 2.0 SaaS account, using
# the driver built above. Optional: skipped without the FIREBOLT_FB2_* secrets
# (forks, unconfigured repositories), and a failure is reported as a warning
# without failing CI, since the engine may be auto-stopped.
if [[ -z "${FIREBOLT_FB2_CLIENT_ID:-}" ]]; then
  printf 'Skipping FB2 live tests: FIREBOLT_FB2_* secrets are not set\n'
  summarize "FB2 live tests" "skipped: no secrets"
elif ! run_phase "FB2 live tests (optional)" \
  "${repo_root}/tests/integration/runner.py" tests/fb2_legacy_live -rs; then
  printf '::warning title=FB2 live tests::tests/fb2_legacy_live failed (optional, does not block the merge)\n'
fi
