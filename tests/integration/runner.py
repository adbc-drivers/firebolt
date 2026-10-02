#!/usr/bin/env python3
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

"""Minimal integration test runner for firebolt-adbc.

Runs pytest inside the firebolt-adbc-integration-test-runner image with libadbc_driver_firebolt.so
bind-mounted in. The test itself spins up a 1-node Firebolt engine via docker compose
from inside the runner container.

Usage:
    runner.py                                          # run all tests
    runner.py -k test_connect                          # forwarded to pytest
    runner.py --engine-image=...:latest -k 'expr' -x   # mix of runner and pytest args

The known runner arguments are --engine-image and --adbc-binary. Everything
else is forwarded to pytest as-is.
"""

import argparse
import logging
import os.path as p
import shlex
import signal
import subprocess
import sys
import uuid

logger = logging.getLogger(__name__)

RUNNER_IMAGE = "firebolt-adbc-integration-test-runner:latest"
# Firebolt engine image: the unified `firebolt` binary (server + client). Its entrypoint
# execs `firebolt <args>` with a default command of `server --data-dir /var/lib/firebolt`.
DEFAULT_ENGINE_IMAGE = "ghcr.io/firebolt-db/engine:latest"

CUR_DIR = p.dirname(p.realpath(__file__))  # adbc/tests/integration
DOCKERFILE_DIR = p.join(CUR_DIR, "docker")  # adbc/tests/integration/docker
REPO_ROOT = p.abspath(p.join(CUR_DIR, "..", ".."))  # adbc
DEFAULT_ADBC_BINARY = p.join(REPO_ROOT, "build", "libadbc_driver_firebolt.so")

_current_container: str | None = None
_current_network: str | None = None


def _sigint_handler(signum, frame):
    if _current_container:
        subprocess.call(["docker", "kill", _current_container])
    if _current_network:
        subprocess.call(
            ["docker", "network", "rm", _current_network],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    raise KeyboardInterrupt("Killed by Ctrl+C")


signal.signal(signal.SIGINT, _sigint_handler)


def _run(cmd: list[str]):
    logger.info("Running: %s", shlex.join(cmd))
    subprocess.check_call(cmd, stdout=sys.stdout, stderr=sys.stderr)


def _ensure_runner_image(image: str):
    """Build the runner image locally from `Dockerfile` if it is not already present.

    The image is intentionally local-only — there is no registry copy. Delete the
    local image (`docker rmi {image}`) to force a rebuild after touching the
    Dockerfile or requirements.txt.
    """
    inspect = subprocess.run(
        ["docker", "image", "inspect", image],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if inspect.returncode == 0:
        logger.info("Runner image %s present locally; skipping build.", image)
        return
    logger.info(
        "Building runner image %s from %s/Dockerfile ...", image, DOCKERFILE_DIR
    )
    _run(["docker", "build", "-t", image, DOCKERFILE_DIR])


def _ensure_image(image: str):
    """Pull `image`, falling back to a locally cached copy when the pull fails.

    The default engine image is a floating tag (`:latest`), so a cached copy can be
    arbitrarily stale — the pull is always attempted first. Offline or unauthenticated
    runs still work as long as a local copy exists: the runner shares the host docker
    socket, so an image pulled on the host is visible to docker compose inside it.
    """
    pull = subprocess.run(
        ["docker", "pull", image],
        stdout=sys.stdout,
        stderr=sys.stderr,
        check=False,
    )
    if pull.returncode == 0:
        return

    inspect = subprocess.run(
        ["docker", "image", "inspect", image],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if inspect.returncode == 0:
        logger.warning("docker pull %s failed; falling back to the local copy.", image)
        return

    raise SystemExit(
        f"docker pull {image} failed and no local copy is available. "
        "Authenticate to the registry (docker login ghcr.io) "
        "or pass --engine-image=<reachable image>."
    )


def _create_bridge_network(network: str, project: str):
    """Create a bridge network with the labels docker compose needs to adopt it."""
    result = subprocess.run(
        [
            "docker",
            "network",
            "create",
            "--driver",
            "bridge",
            "--label",
            f"com.docker.compose.project={project}",
            "--label",
            "com.docker.compose.network=default",
            "--label",
            "com.docker.compose.version=2",
            network,
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0 and "already exists" not in result.stderr:
        raise RuntimeError(
            f"Failed to create Docker network {network}: {result.stderr.strip()}"
        )


def _remove_bridge_network(network: str):
    subprocess.run(
        ["docker", "network", "rm", network],
        stderr=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        check=False,
    )


def _launch_runner(
    engine_image: str,
    adbc_binary: str,
    pytest_args: list[str],
    project: str,
    network: str,
) -> int:
    global _current_container, _current_network
    container = f"{project}_pytest"
    _current_container = container
    _current_network = network

    adbc_so = p.abspath(adbc_binary)
    if not p.isfile(adbc_so):
        raise SystemExit(
            f"libadbc_driver_firebolt.so not found at {adbc_so}. Build it first:\n"
            "  cmake --preset standalone-clang -S .\n"
            "  cmake --build build -j$(nproc)\n"
            "or pass --adbc-binary=<path>."
        )

    workdir = p.join(REPO_ROOT, "tests", "integration")
    tty_flags = ["-it"] if sys.stdout.isatty() and sys.stdin.isatty() else []

    pytest_cmd = ["pytest", "-ss", "-rfEp", "--color=no", "-vv"] + pytest_args

    cmd = (
        [
            "docker",
            "run",
            f"--network={network}",
            "--rm",
            "--name",
            container,
            "--privileged",
            f"--volume={REPO_ROOT}:{REPO_ROOT}",
            "--volume=/var/run/docker.sock:/var/run/docker.sock",
            f"--volume={adbc_so}:{adbc_so}",
            f"--workdir={workdir}",
            "-e",
            f"PACKDB_TESTS_ADBC_BINARY_PATH={adbc_so}",
            "-e",
            f"FIREBOLT_ENGINE_IMAGE={engine_image}",
            "-e",
            f"COMPOSE_PROJECT_NAME={project}",
        ]
        + tty_flags
        + [
            RUNNER_IMAGE,
            # The container entrypoint joins all args with spaces and runs them
            # under sh -c, so pass the whole pytest invocation as one shlex-quoted
            # string so argument boundaries (e.g. `-k 'foo and bar'`) survive.
            shlex.join(pytest_cmd),
        ]
    )

    logger.info("Launching runner container %s: %s", container, shlex.join(cmd))
    return subprocess.Popen(cmd, stdout=sys.stdout, stderr=sys.stderr).wait()


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Minimal firebolt-adbc integration test runner. Builds "
            f"{RUNNER_IMAGE} locally if missing, pulls the chosen engine image, "
            "creates a bridge network, and runs pytest inside the runner container.\n\n"
            "Unrecognised arguments are forwarded to pytest as-is, e.g.:\n"
            "  ./runner.py -k test_connect\n"
            "  ./runner.py --engine-image=...:latest -x tests/adbc_sanity"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--engine-image",
        default=DEFAULT_ENGINE_IMAGE,
        help=f"Firebolt engine Docker image (default: {DEFAULT_ENGINE_IMAGE})",
    )
    parser.add_argument(
        "--adbc-binary",
        default=DEFAULT_ADBC_BINARY,
        help=f"Path to libadbc_driver_firebolt.so (default: {DEFAULT_ADBC_BINARY})",
    )
    args, pytest_args = parser.parse_known_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")

    run_id = uuid.uuid4().hex[:4]
    project = f"fbadbc_{run_id}"
    network = f"{project}_default"

    print(f"Run ID: {run_id}")
    print(f"Engine image: {args.engine_image}")
    print(f"ADBC binary: {args.adbc_binary}")
    print(f"Pytest args: {pytest_args}")

    _ensure_runner_image(RUNNER_IMAGE)
    _ensure_image(args.engine_image)

    _create_bridge_network(network, project)
    try:
        retcode = _launch_runner(
            args.engine_image, args.adbc_binary, pytest_args, project, network
        )
    finally:
        _remove_bridge_network(network)

    if retcode != 0:
        raise SystemExit(retcode)
    logger.info("Tests passed successfully")


if __name__ == "__main__":
    main()
