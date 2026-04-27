# Minimal integration-test runner image for firebolt-adbc.
#
# Provides Python + pytest + adbc-driver-manager + docker CLI so that
# adbc/tests/integration/runner/runner.py can spin up a 1-node Firebolt Core
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
