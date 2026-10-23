# Nexora OS build environment.
#
#   docker build -t nexora-os-build .
#   docker run --rm -it -v "$PWD":/src -w /src nexora-os-build make iso
#
# scripts/build.sh wraps both steps. The LLVM/Rust/Limine versions come from
# scripts/versions.env via scripts/setup-toolchain.sh, the same script native builds use.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git xz-utils make python3 \
        gcc libc6-dev \
        nasm xorriso mtools gdisk dosfstools \
        qemu-system-x86 ovmf \
    && rm -rf /var/lib/apt/lists/*

ENV HZ_TOOLCHAIN=/opt/nexora-os-toolchain
COPY scripts/versions.env scripts/setup-toolchain.sh /tmp/hz/scripts/
# Builds run as the host uid (scripts/build.sh --user), and cargo/rustup write lock and
# settings files into their homes, so those two directories are world-writable.
RUN bash /tmp/hz/scripts/setup-toolchain.sh "$HZ_TOOLCHAIN" && rm -rf /tmp/hz \
    && chmod -R a+rX "$HZ_TOOLCHAIN" \
    && chmod -R a+rwX "$HZ_TOOLCHAIN/cargo" "$HZ_TOOLCHAIN/rustup"

RUN apt-get update && apt-get install -y --no-install-recommends libxml2 \
    && rm -rf /var/lib/apt/lists/*

RUN apt-get update && apt-get install -y --no-install-recommends python3-pil \
    && rm -rf /var/lib/apt/lists/*

ENV RUSTUP_HOME=/opt/nexora-os-toolchain/rustup \
    CARGO_HOME=/opt/nexora-os-toolchain/cargo \
    PATH=/opt/nexora-os-toolchain/llvm/bin:/opt/nexora-os-toolchain/cargo/bin:$PATH

# The source tree is bind-mounted and may be owned by another uid.
RUN git config --system --add safe.directory '*'

WORKDIR /src
CMD ["make", "iso"]
