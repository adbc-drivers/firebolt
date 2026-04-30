# firebolt-adbc-builder
#
# Build environment for libfirebolt_adbc.so.  Ubuntu 22.04 is pinned so the
# resulting binary links against the older glibc (2.35) shipped with jammy and
# remains forward-compatible with newer host systems.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# clang-18 is not in jammy's default archive, so we add the LLVM apt repo.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg \
        build-essential cmake ninja-build git zlib1g-dev \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
        | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        clang-18 lld-18 \
    && rm -rf /var/lib/apt/lists/*
