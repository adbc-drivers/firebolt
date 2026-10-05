<!--
  Copyright (c) 2026 ADBC Drivers Contributors

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

          http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
-->

# Validation Suite Setup

Build the driver for the host, start the Firebolt service, and run the shared ADBC
validation suite:

```shell
pixi run make
docker compose up --detach --wait test-service
FIREBOLT_URI=http://localhost:3473 pixi run validate
docker compose down --volumes
```

`compose.yaml` starts a single-node engine with authentication disabled and
creates the secondary catalog and schema required by the metadata and ingest
tests.

The suite uses the host's native driver and Pixi environment. Pass extra pytest
arguments directly, for example:

```shell
FIREBOLT_URI=http://localhost:3473 pixi run validate --vendor-version 5.0.0 -k get_objects
```

The generated Linux validation job runs this same sequence for Firebolt 5.0.0.
Its configuration lives in `.github/workflows/generate.toml`; do not edit the
generated workflow YAML directly.
