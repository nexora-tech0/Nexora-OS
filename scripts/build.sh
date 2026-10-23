#!/usr/bin/env bash
# Build Nexora OS. Runs make inside the Docker build container when Docker is available,
# otherwise natively with the pinned toolchain from scripts/setup-toolchain.sh.
#
#   scripts/build.sh [make targets/variables...]     (default target: iso)
#
# Environment:
#   HZ_BUILD_MODE=docker|native   force one mode (default: docker if usable, else native)
#   HZ_DOCKER_IMAGE               image tag (default: nexora-os-build:<hash of build inputs>)
#
# Targets that start QEMU with a window (run) or drive VirtualBox (vbox) must run on the host;
# in Docker mode build the ISO here, then use `make run` / `make vbox` natively.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
[ $# -gt 0 ] || set -- iso

mode="${HZ_BUILD_MODE:-}"
if [ -z "$mode" ]; then
    if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        mode=docker
    else
        mode=native
    fi
fi

case "$mode" in
docker)
    hash="$(cat Dockerfile scripts/versions.env scripts/setup-toolchain.sh | sha256sum | cut -c1-12)"
    image="${HZ_DOCKER_IMAGE:-nexora-os-build:$hash}"
    if ! docker image inspect "$image" >/dev/null 2>&1; then
        echo "[build] building Docker image $image"
        docker build -t "$image" .
    fi
    tty_flags=()
    [ -t 0 ] && [ -t 1 ] && tty_flags=(-it)
    echo "[build] docker: make $*"
    exec docker run --rm "${tty_flags[@]}" \
        --user "$(id -u):$(id -g)" -e HOME=/tmp \
        -v "$ROOT":/src -w /src "$image" make "$@"
    ;;
native)
    toolchain="${HZ_TOOLCHAIN:-$HOME/.nexora-os-toolchain}"
    if [ ! -x "$toolchain/llvm/bin/clang" ]; then
        echo "[build] pinned toolchain not found in $toolchain; installing it (no root needed)"
        scripts/setup-toolchain.sh "$toolchain"
    fi
    echo "[build] native: make $*"
    exec make HZ_TOOLCHAIN="$toolchain" "$@"
    ;;
*)
    echo "build.sh: unknown HZ_BUILD_MODE '$mode'" >&2
    exit 1
    ;;
esac
