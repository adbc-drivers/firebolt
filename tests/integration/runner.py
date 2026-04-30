#!/usr/bin/env python3
"""Minimal integration test runner for firebolt-adbc.

Runs pytest inside the firebolt-adbc-integration-test-runner image with libfirebolt_adbc.so
bind-mounted in. The test itself spins up a 1-node Firebolt Core via docker compose
from inside the runner container.

Usage:
    runner.py                                         # run all tests
    runner.py -k test_connect                         # forwarded to pytest
    runner.py --core-image=...:latest -k 'expr' -x    # mix of runner and pytest args

The known runner arguments are --core-image and --adbc-binary. Everything
else is forwarded to pytest as-is.
"""

import argparse
import logging
import os
import os.path as p
import shlex
import signal
import subprocess
import sys
import uuid


RUNNER_IMAGE = "firebolt-adbc-integration-test-runner:latest"
DEFAULT_CORE_IMAGE = "ghcr.io/firebolt-db/firebolt-core:4.32.0-pre.0.20260429090644.542714fe5ef7"

CUR_DIR = p.dirname(p.realpath(__file__))                        # adbc/tests/integration
DOCKERFILE_DIR = p.join(CUR_DIR, "docker")                       # adbc/tests/integration/docker
REPO_ROOT = p.abspath(p.join(CUR_DIR, "..", ".."))               # adbc
DEFAULT_ADBC_BINARY = p.join(REPO_ROOT, "build", "libfirebolt_adbc.so")

_current_container: str | None = None
_current_network: str | None = None


def _sigint_handler(signum, frame):
    if _current_container:
        subprocess.call(["docker", "kill", _current_container])
    if _current_network:
        subprocess.call(
            ["docker", "network", "rm", _current_network],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    raise KeyboardInterrupt("Killed by Ctrl+C")


signal.signal(signal.SIGINT, _sigint_handler)


def _run(cmd: list[str]):
    logging.info("Running: %s", shlex.join(cmd))
    subprocess.check_call(cmd, stdout=sys.stdout, stderr=sys.stderr)


def _ensure_runner_image(image: str):
    """Build the runner image locally from `Dockerfile` if it is not already present.

    The image is intentionally local-only — there is no registry copy. Delete the
    local image (`docker rmi {image}`) to force a rebuild after touching the
    Dockerfile or requirements.txt.
    """
    inspect = subprocess.run(
        ["docker", "image", "inspect", image],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if inspect.returncode == 0:
        logging.info("Runner image %s present locally; skipping build.", image)
        return
    logging.info("Building runner image %s from %s/Dockerfile ...", image, DOCKERFILE_DIR)
    _run(["docker", "build", "-t", image, DOCKERFILE_DIR])


def _ensure_image(image: str):
    """Skip the pull when `image` is already cached locally; otherwise pull.

    The Firebolt Core image lives in a private ECR repo that may not be reachable
    without AWS credentials. The runner shares the host docker socket, so a copy
    already pulled on the host is visible to docker compose inside the runner.
    """
    inspect = subprocess.run(
        ["docker", "image", "inspect", image],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if inspect.returncode == 0:
        logging.info("Core image %s present locally; skipping pull.", image)
        return

    pull = subprocess.run(
        ["docker", "pull", image],
        stdout=sys.stdout, stderr=sys.stderr,
    )
    if pull.returncode == 0:
        return
    raise SystemExit(
        f"docker pull {image} failed and no local copy is available. "
        "Authenticate to the registry (aws ecr get-login-password ... | docker login) "
        "or pass --core-image=<reachable image>."
    )


def _create_bridge_network(network: str, project: str):
    """Create a bridge network with the labels docker compose needs to adopt it."""
    result = subprocess.run(
        [
            "docker", "network", "create",
            "--driver", "bridge",
            "--label", f"com.docker.compose.project={project}",
            "--label", "com.docker.compose.network=default",
            "--label", "com.docker.compose.version=2",
            network,
        ],
        stderr=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
    )
    if result.returncode != 0 and "already exists" not in result.stderr:
        raise RuntimeError(f"Failed to create Docker network {network}: {result.stderr.strip()}")


def _remove_bridge_network(network: str):
    subprocess.run(
        ["docker", "network", "rm", network],
        stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
    )


def _launch_runner(
    core_image: str, adbc_binary: str, pytest_args: list[str], project: str, network: str,
) -> int:
    global _current_container, _current_network
    container = f"{project}_pytest"
    _current_container = container
    _current_network = network

    adbc_so = p.abspath(adbc_binary)
    if not p.isfile(adbc_so):
        raise SystemExit(
            f"libfirebolt_adbc.so not found at {adbc_so}. Build it first:\n"
            "  cmake --preset standalone-clang -S .\n"
            "  cmake --build build -j$(nproc)\n"
            "or pass --adbc-binary=<path>."
        )

    workdir = p.join(REPO_ROOT, "tests", "integration")
    tty_flags = ["-it"] if sys.stdout.isatty() and sys.stdin.isatty() else []

    pytest_cmd = ["pytest", "-ss", "-rfEp", "--color=no", "-vv"] + pytest_args

    cmd = [
        "docker", "run",
        f"--network={network}",
        "--rm",
        "--name", container,
        "--privileged",
        f"--volume={REPO_ROOT}:{REPO_ROOT}",
        "--volume=/var/run/docker.sock:/var/run/docker.sock",
        f"--volume={adbc_so}:{adbc_so}",
        f"--workdir={workdir}",
        "-e", f"PACKDB_TESTS_ADBC_BINARY_PATH={adbc_so}",
        "-e", f"FIREBOLT_CORE_IMAGE={core_image}",
        "-e", f"COMPOSE_PROJECT_NAME={project}",
    ] + tty_flags + [
        RUNNER_IMAGE,
        # The container entrypoint joins all args with spaces and runs them
        # under sh -c, so pass the whole pytest invocation as one shlex-quoted
        # string so argument boundaries (e.g. `-k 'foo and bar'`) survive.
        shlex.join(pytest_cmd),
    ]

    logging.info("Launching runner container %s: %s", container, shlex.join(cmd))
    return subprocess.Popen(cmd, stdout=sys.stdout, stderr=sys.stderr).wait()


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Minimal firebolt-adbc integration test runner. Builds "
            f"{RUNNER_IMAGE} locally if missing, pulls the chosen firebolt-core image, "
            "creates a bridge network, and runs pytest inside the runner container.\n\n"
            "Unrecognised arguments are forwarded to pytest as-is, e.g.:\n"
            "  ./runner.py -k test_connect\n"
            "  ./runner.py --core-image=...:latest -x tests/adbc_sanity"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--core-image", default=DEFAULT_CORE_IMAGE,
        help=f"Firebolt Core Docker image (default: {DEFAULT_CORE_IMAGE})",
    )
    parser.add_argument(
        "--adbc-binary", default=DEFAULT_ADBC_BINARY,
        help=f"Path to libfirebolt_adbc.so (default: {DEFAULT_ADBC_BINARY})",
    )
    args, pytest_args = parser.parse_known_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")

    run_id = uuid.uuid4().hex[:4]
    project = f"fbadbc_{run_id}"
    network = f"{project}_default"

    print(f"Run ID: {run_id}")
    print(f"Core image: {args.core_image}")
    print(f"ADBC binary: {args.adbc_binary}")
    print(f"Pytest args: {pytest_args}")

    _ensure_runner_image(RUNNER_IMAGE)
    _ensure_image(args.core_image)

    _create_bridge_network(network, project)
    try:
        retcode = _launch_runner(args.core_image, args.adbc_binary, pytest_args, project, network)
    finally:
        _remove_bridge_network(network)

    if retcode != 0:
        raise SystemExit(retcode)
    logging.info("Tests passed successfully")


if __name__ == "__main__":
    main()
