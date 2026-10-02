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

"""Minimal Firebolt engine fixture for the standalone firebolt-adbc repo.

A trimmed port of packdb's tests/integration/helpers/firebolt_instance.py that
launches a single-node engine container via docker compose and exposes its
container IP through engines["engine1"].instances["node_1"].pg_host.

The container image (``ghcr.io/firebolt-db/engine``) ships the unified
``firebolt`` binary: its entrypoint execs ``firebolt <args>`` and its default
command is ``server --data-dir /var/lib/firebolt``.  With no config file
supplied the server builds its configuration from the built-in structured
(YAML) defaults — a single node listening on all interfaces on the default
ports — which is exactly what a 1-node fixture needs, so no config file is
written or mounted here.  (Multi-node would need a ``config.yaml`` bind-mounted
at ``/var/lib/firebolt/config.yaml``; the legacy ``--node N`` command form with
``/firebolt-core/config.json`` is gone.)
"""

import json
import os
import os.path as p
import shutil
import subprocess
import time
import uuid

import requests

QUERY_PORT = 3473


class FireboltEngineNode:
    def __init__(self, engine_name: str, node_index: int):
        self.node_index = node_index
        self.node_name = f"{engine_name}-node-{node_index}"
        self.docker_container_name = f"{engine_name}-{engine_name}-node-{node_index}-1"
        self.pg_host: str | None = None


class FireboltEngine:
    def __init__(self, engine_name: str, engine_directory: str, network_name: str):
        self.name = engine_name
        self.network_name = network_name
        self.engine_directory = engine_directory

        self.log_directory = p.join(engine_directory, "_instances")
        shutil.rmtree(self.log_directory, ignore_errors=True)
        os.makedirs(self.log_directory, exist_ok=True)

        docker_directory = p.join(engine_directory, "docker")
        shutil.rmtree(docker_directory, ignore_errors=True)
        os.makedirs(docker_directory, exist_ok=True)

        self.compose_path = p.join(
            docker_directory, "firebolt_engine_docker_compose.yml"
        )

        self.is_running = False
        self.instances: dict[str, FireboltEngineNode] = {
            "node_1": FireboltEngineNode(engine_name, 0),
        }
        os.makedirs(p.join(self.log_directory, "node_1"), exist_ok=True)


class FireboltInstance:
    def __init__(self, base_path: str):
        self.image = os.environ.get("FIREBOLT_ENGINE_IMAGE")
        assert self.image, (
            "FIREBOLT_ENGINE_IMAGE is not set. Run via runner.py (which sets it from "
            "--engine-image) or export FIREBOLT_ENGINE_IMAGE=<image> before invoking pytest."
        )
        self.base_dir = p.dirname(base_path)
        self.runtime_root_dir = p.join(self.base_dir, "_test_runtime_root")
        os.makedirs(self.runtime_root_dir, exist_ok=True)

        self.engines: dict[str, FireboltEngine] = {}
        self.project_name = "firebolt-engine_" + uuid.uuid4().hex[:8]

    def __del__(self):
        for engine in list(self.engines.values()):
            try:
                self._stop_engine(engine)
            except Exception as e:  # noqa: BLE001 -- __del__ must not raise; report and move on
                print(f"Error stopping engine {engine.name}: {e}")

    @staticmethod
    def _exec(args, **kwargs):
        print(f"run command {args}")
        subprocess.run(args, check=True, **kwargs)

    @staticmethod
    def _exec_capture(args, **kwargs):
        print(f"run command {args}")
        return subprocess.run(
            args, stdout=subprocess.PIPE, text=True, check=True, **kwargs
        ).stdout

    @staticmethod
    def _runner_container_name() -> str | None:
        compose_project = os.environ.get("COMPOSE_PROJECT_NAME")
        return f"{compose_project}_pytest" if compose_project else None

    def _create_network(self, engine_name: str) -> str:
        network_name = f"firebolt-engine-{engine_name}-{self.project_name}"
        print(f"Ensuring Docker network {network_name} exists.")
        inspect = subprocess.run(
            ["docker", "network", "inspect", network_name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if inspect.returncode != 0:
            self._exec(["docker", "network", "create", network_name])

        # When running inside a runner container, attach it so it can reach the
        # engine node by IP on the engine network.
        runner = self._runner_container_name()
        if runner:
            subprocess.call(["docker", "network", "connect", network_name, runner])

        return network_name

    def _remove_network(self, network_name: str):
        runner = self._runner_container_name()
        if runner:
            subprocess.call(["docker", "network", "disconnect", network_name, runner])
        subprocess.call(["docker", "network", "rm", network_name])

    def _write_compose_file(self, engine: FireboltEngine, node_count: int):
        # No `command:` override — the image default (`server --data-dir
        # /var/lib/firebolt`) is what we want, and the data dir is the image's
        # own VOLUME (dropped again by `compose down -v` at teardown).
        compose = "services:\n"
        for node_index in range(node_count):
            compose += f"""
    {engine.name}-node-{node_index}:
        image: {self.image}
        tty: true
        security_opt:
            - seccomp:unconfined
        ulimits:
            memlock: 8589934592
        networks:
            - {engine.network_name}
"""
        compose += f"""
networks:
    {engine.network_name}:
        external: true
"""
        print(f"Creating '{engine.name}' Docker compose file at {engine.compose_path}.")
        with open(engine.compose_path, "w") as f:
            f.write(compose)

    def add_engine(self, engine_name: str = "engine1", num_nodes: int = 1):
        assert engine_name not in self.engines, f"Engine '{engine_name}' already exists"
        assert num_nodes == 1, "Minimal FireboltInstance only supports num_nodes=1"

        network_name = self._create_network(engine_name)
        engine_directory = p.join(self.runtime_root_dir, engine_name)
        engine = FireboltEngine(engine_name, engine_directory, network_name)
        self.engines[engine_name] = engine

        self._write_compose_file(engine, num_nodes)

    def _dump_container_logs(self, node: FireboltEngineNode, tail: int = 200):
        result = subprocess.run(
            ["docker", "logs", "--tail", str(tail), node.docker_container_name],
            capture_output=True,
            text=True,
            check=False,
        )
        print(
            f"Container {node.docker_container_name} failed to become ready. "
            f"Last {tail} lines:\nSTDOUT:\n{result.stdout[-5000:] or '(empty)'}\n"
            f"STDERR:\n{result.stderr[-5000:] or '(empty)'}"
        )

    @staticmethod
    def _ping(base_url: str) -> None:
        r = requests.get(f"{base_url}/ping", timeout=5)
        if r.status_code != 200:
            raise RuntimeError(
                f"GET {base_url}/ping -> {r.status_code}: {r.text[:200]}"
            )

    @staticmethod
    def _select_one(base_url: str) -> None:
        """Run `SELECT 1` over the query endpoint.

        /ping turns green before the engine can serve queries ("Cluster not yet
        healthy"), so readiness has to include an actual query — the same reason
        packdb's fixture follows /ping with a cluster-health probe.
        """
        r = requests.post(
            base_url,
            params={"output_format": "JSON_Compact"},
            data="SELECT 1",
            timeout=10,
        )
        if r.status_code != 200:
            raise RuntimeError(
                f"POST {base_url} 'SELECT 1' -> {r.status_code}: {r.text[:200]}"
            )

    def _wait_until(
        self,
        probe,
        base_url: str,
        what: str,
        node: FireboltEngineNode,
        timeout: float,
        interval: float,
    ):
        deadline = time.time() + timeout
        last_err: Exception | None = None
        while time.time() < deadline:
            try:
                probe(base_url)
                return
            except (requests.RequestException, RuntimeError) as e:
                last_err = e
            time.sleep(interval)
        self._dump_container_logs(node)
        raise TimeoutError(
            f"Firebolt engine node not ready ({what}) at {base_url}: {last_err}"
        )

    def start(self, timeout: float = 120.0, interval: float = 2.0):
        for engine in self.engines.values():
            print(f"Start Firebolt engine cluster {engine.name}")
            self._exec(
                [
                    "docker",
                    "compose",
                    "-p",
                    engine.name,
                    "-f",
                    engine.compose_path,
                    "up",
                    "-d",
                ]
            )
            # Mark running as soon as compose has created containers, before the
            # inspect loop below, so that a failure while collecting endpoints
            # still triggers compose down + network rm in _stop_engine instead
            # of leaking the stack.
            engine.is_running = True

            print(f"Collecting endpoints for Firebolt engine {engine.name}")
            for node in engine.instances.values():
                containers = json.loads(
                    self._exec_capture(
                        ["docker", "container", "inspect", node.docker_container_name]
                    )
                )
                ip = containers[0]["NetworkSettings"]["Networks"][engine.network_name][
                    "IPAddress"
                ]
                node.pg_host = ip

        for engine in self.engines.values():
            for node in engine.instances.values():
                base_url = f"http://{node.pg_host}:{QUERY_PORT}"
                self._wait_until(self._ping, base_url, "/ping", node, timeout, interval)
                self._wait_until(
                    self._select_one, base_url, "SELECT 1", node, timeout, interval
                )

    def _stop_engine(self, engine: FireboltEngine):
        if not engine.is_running:
            return

        print(f"Save logs from Firebolt engine {engine.name}")
        for node in engine.instances.values():
            target = f"{engine.log_directory}/node_{node.node_index + 1}/engine.log"
            print(f"Save logs from container {node.docker_container_name} in {target}")
            subprocess.run(
                f'docker logs "{node.docker_container_name}" '
                f"| sed 's/\x1b\\[[0-9;]*m//g' > \"{target}\"",
                shell=True,
                check=False,
            )

        print(f"Stop Firebolt engine {engine.name}")
        subprocess.run(
            [
                "docker",
                "compose",
                "-p",
                engine.name,
                "-f",
                engine.compose_path,
                # -v: the image declares /var/lib/firebolt as a VOLUME, so every run
                # creates an anonymous data volume that would otherwise be leaked.
                "down",
                "-v",
            ],
            check=False,
        )

        self._remove_network(engine.network_name)
        engine.is_running = False

    def stop(self):
        for engine in self.engines.values():
            self._stop_engine(engine)
