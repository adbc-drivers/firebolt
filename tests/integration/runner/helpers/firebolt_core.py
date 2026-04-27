"""Minimal Firebolt Core fixture for the standalone firebolt-adbc repo.

A trimmed port of packdb's tests/integration/helpers/firebolt_core.py that
launches a single-node Firebolt Core container via docker compose and exposes
its container IP through engines["engine1"].instances["node_1"].pg_host.

Drops everything the ADBC sanity test does not use: pensieve, ducklake, kafka,
minio, multi-node, network isolation, unix sockets, custom configs, log-line
waiting, and the helpers.utils package dependency. Readiness is checked with a
simple HTTP GET on /ping instead of helpers.utils.packdb.HTTPApi.
"""

import json
import os
import os.path as p
import shutil
import subprocess
import time
import uuid

import requests


FIREBOLT_CORE_QUERY_ENDPOINT = 3473


class FireboltCoreNode:
    def __init__(self, engine_name: str, node_index: int):
        self.node_index = node_index
        self.node_name = f"{engine_name}-node-{node_index}"
        self.docker_container_name = f"{engine_name}-{engine_name}-node-{node_index}-1"
        self.pg_host: str | None = None


class FireboltCoreEngine:
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

        self.core_compose_path = p.join(docker_directory, "firebolt_core_docker_compose.yml")
        self.config_json_path = p.join(docker_directory, "firebolt_core_config.json")

        self.is_running = False
        self.instances: dict[str, FireboltCoreNode] = {
            "node_1": FireboltCoreNode(engine_name, 0),
        }
        os.makedirs(p.join(self.log_directory, "node_1"), exist_ok=True)


class FireboltCore:
    def __init__(self, base_path: str):
        self.image = os.environ.get("FIREBOLT_CORE_IMAGE")
        assert self.image, (
            "FIREBOLT_CORE_IMAGE is not set. Run via runner.py (which sets it from "
            "--core-image) or export FIREBOLT_CORE_IMAGE=<image> before invoking pytest."
        )
        self.base_dir = p.dirname(base_path)
        self.runtime_root_dir = p.join(self.base_dir, "_test_runtime_root")
        os.makedirs(self.runtime_root_dir, exist_ok=True)

        self.engines: dict[str, FireboltCoreEngine] = {}
        self.project_name = "firebolt-core_" + uuid.uuid4().hex[:8]

    def __del__(self):
        for engine in list(self.engines.values()):
            try:
                self._stop_engine(engine)
            except Exception as e:
                print(f"Error stopping engine {engine.name}: {e}")

    @staticmethod
    def _exec(args, **kwargs):
        print(f"run command {args}")
        subprocess.run(args, check=True, **kwargs)

    @staticmethod
    def _exec_capture(args, **kwargs):
        print(f"run command {args}")
        return subprocess.run(args, stdout=subprocess.PIPE, text=True, check=True, **kwargs).stdout

    @staticmethod
    def _runner_container_name() -> str | None:
        compose_project = os.environ.get("COMPOSE_PROJECT_NAME")
        return f"{compose_project}_pytest" if compose_project else None

    def _create_network(self, engine_name: str) -> str:
        network_name = f"firebolt-core-{engine_name}-{self.project_name}"
        print(f"Ensuring Docker network {network_name} exists.")
        inspect = subprocess.run(
            ["docker", "network", "inspect", network_name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if inspect.returncode != 0:
            self._exec(["docker", "network", "create", network_name])

        # When running inside a runner container, attach it so it can reach the
        # firebolt-core node by IP on the engine network.
        runner = self._runner_container_name()
        if runner:
            subprocess.call(["docker", "network", "connect", network_name, runner])

        return network_name

    def _remove_network(self, network_name: str):
        runner = self._runner_container_name()
        if runner:
            subprocess.call(["docker", "network", "disconnect", network_name, runner])
        subprocess.call(["docker", "network", "rm", network_name])

    def _write_compose_file(self, engine: FireboltCoreEngine, node_count: int):
        compose = "services:\n"
        for node_index in range(node_count):
            compose += f"""
    {engine.name}-node-{node_index}:
        image: {self.image}
        command: --node {node_index}
        tty: true
        security_opt:
            - seccomp:unconfined
        ulimits:
            memlock: 8589934592
        volumes:
            - {engine.config_json_path}:/firebolt-core/config.json:ro
        networks:
            - {engine.network_name}
"""
        compose += f"""
networks:
    {engine.network_name}:
        external: true
"""
        print(f"Creating '{engine.name}' Docker compose file at {engine.core_compose_path}.")
        with open(engine.core_compose_path, "w") as f:
            f.write(compose)

    def add_engine(self, engine_name: str = "engine1", num_nodes: int = 1):
        assert engine_name not in self.engines, f"Engine '{engine_name}' already exists"
        assert num_nodes == 1, "Minimal FireboltCore only supports num_nodes=1"

        network_name = self._create_network(engine_name)
        engine_directory = p.join(self.runtime_root_dir, engine_name)
        engine = FireboltCoreEngine(engine_name, engine_directory, network_name)
        self.engines[engine_name] = engine

        self._write_compose_file(engine, num_nodes)

        config = {"nodes": [{"host": f"{engine_name}-node-{i}"} for i in range(num_nodes)]}
        with open(engine.config_json_path, "w") as f:
            json.dump(config, f)

    def _dump_container_logs(self, node: FireboltCoreNode, tail: int = 200):
        result = subprocess.run(
            ["docker", "logs", "--tail", str(tail), node.docker_container_name],
            capture_output=True, text=True,
        )
        print(
            f"Container {node.docker_container_name} failed to become ready. "
            f"Last {tail} lines:\nSTDOUT:\n{result.stdout[-5000:] or '(empty)'}\n"
            f"STDERR:\n{result.stderr[-5000:] or '(empty)'}"
        )

    def start(self, timeout: float = 120.0, interval: float = 2.0):
        for engine in self.engines.values():
            print(f"Start Firebolt Core Cluster {engine.name}")
            self._exec([
                "docker", "compose",
                "-p", engine.name,
                "-f", engine.core_compose_path,
                "up", "-d",
            ])

            print(f"Collecting endpoints for Firebolt Core engine {engine.name}")
            for node in engine.instances.values():
                containers = json.loads(self._exec_capture(
                    ["docker", "container", "inspect", node.docker_container_name]
                ))
                ip = containers[0]["NetworkSettings"]["Networks"][engine.network_name]["IPAddress"]
                node.pg_host = ip

            engine.is_running = True

        for engine in self.engines.values():
            for node in engine.instances.values():
                url = f"http://{node.pg_host}:{FIREBOLT_CORE_QUERY_ENDPOINT}/ping"
                deadline = time.time() + timeout
                last_err: Exception | None = None
                while time.time() < deadline:
                    try:
                        r = requests.get(url, timeout=5)
                        if r.status_code == 200:
                            last_err = None
                            break
                        last_err = RuntimeError(f"GET {url} -> {r.status_code}: {r.text[:200]}")
                    except Exception as e:
                        last_err = e
                    time.sleep(interval)
                if last_err is not None:
                    self._dump_container_logs(node)
                    raise TimeoutError(f"Firebolt Core node not ready at {url}: {last_err}")

    def _stop_engine(self, engine: FireboltCoreEngine):
        if not engine.is_running:
            return

        print(f"Save logs from Firebolt Core Engine {engine.name}")
        for node in engine.instances.values():
            target = f"{engine.log_directory}/node_{node.node_index + 1}/core.log"
            print(f"Save logs from container {node.docker_container_name} in {target}")
            subprocess.run(
                f"docker logs \"{node.docker_container_name}\" "
                f"| sed 's/\x1b\\[[0-9;]*m//g' > \"{target}\"",
                shell=True,
            )

        print(f"Stop Firebolt Core Engine {engine.name}")
        subprocess.run([
            "docker", "compose",
            "-p", engine.name,
            "-f", engine.core_compose_path,
            "down",
        ], check=False)

        self._remove_network(engine.network_name)
        engine.is_running = False

    def stop(self):
        for engine in self.engines.values():
            self._stop_engine(engine)
