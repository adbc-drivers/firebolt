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

# Run the pytest-based integration suite against a 1-node Firebolt engine.
#
# Forwards extra args to runner.py, e.g.
#   scripts/test-integration.sh --engine-image=...:latest -k test_connect
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
exec "$REPO_ROOT/tests/integration/runner.py" "$@"
