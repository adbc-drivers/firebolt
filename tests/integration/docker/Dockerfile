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

# Minimal integration-test runner image for firebolt-adbc.
#
# Provides Python + pytest + adbc-driver-manager + docker CLI so that
# adbc/tests/integration/runner.py can spin up a 1-node Firebolt engine
# via docker compose from inside the container.
#
# Built on demand by runner.py when firebolt-adbc-integration-test-runner:latest
# is not already present locally.
FROM ubuntu:24.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        python3 \
        python3-pip \
        docker.io \
        docker-compose-v2 \
    && rm -rf /var/lib/apt/lists/*

COPY requirements.txt /tmp/requirements.txt
RUN python3 -m pip install --break-system-packages -r /tmp/requirements.txt

# runner.py passes the entire pytest invocation as a single shlex-quoted string,
# so the entrypoint must hand it to a shell parser.
ENTRYPOINT ["/bin/sh", "-c"]
