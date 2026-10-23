#!/usr/bin/env bash
# Install the pinned Nexora OS cross toolchain into a self-contained prefix, without root.
#
#   scripts/setup-toolchain.sh [PREFIX]      (default: $HZ_TOOLCHAIN or ~/.nexora-os-toolchain)
#
# Installs:  PREFIX/llvm    LLVM/Clang/LLD  (LLVM_VERSION)
#            PREFIX/rustup  PREFIX/cargo    Rust (RUST_VERSION) + x86_64-unknown-none
#            PREFIX/limine  Limine UEFI binaries (LIMINE_TAG, verified against LIMINE_COMMIT)
# Used by the Dockerfile and by native (WSL2 / Linux) builds, so both get identical versions.
# System packages still required: nasm xorriso mtools gdisk dosfstools make git curl xz-utils, plus a host C
# compiler with libc headers; qemu-system-x86 + ovmf for `make run`.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=versions.env
. "$ROOT/scripts/versions.env"

PREFIX="${1:-${HZ_TOOLCHAIN:-$HOME/.nexora-os-toolchain}}"
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"
STAMP_DIR="$PREFIX/.stamps"
mkdir -p "$STAMP_DIR"

log() { printf '\033[1;36m[toolchain]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[toolchain] error:\033[0m %s\n' "$*" >&2; exit 1; }

for tool in curl tar xz git; do
    command -v "$tool" >/dev/null 2>&1 || die "'$tool' is required (install it with your package manager)"
done

# ---------------------------------------------------------------------------------------------
# LLVM
# ---------------------------------------------------------------------------------------------
llvm_stamp="$STAMP_DIR/llvm-$LLVM_VERSION-min"
if [ ! -f "$llvm_stamp" ]; then
    url="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/$LLVM_TARBALL"
    tarball="$PREFIX/$LLVM_TARBALL"
    if [ ! -f "$tarball.done" ]; then
        log "downloading $url"
        curl -L --fail --retry 3 -o "$tarball" "$url"
        touch "$tarball.done"
    fi
    actual="$(sha256sum "$tarball" | cut -d' ' -f1)"
    if [ -n "${LLVM_SHA256:-}" ]; then
        [ "$actual" = "$LLVM_SHA256" ] || die "LLVM tarball sha256 mismatch: got $actual, want $LLVM_SHA256"
    else
        log "LLVM tarball sha256 = $actual (pin it as LLVM_SHA256 in scripts/versions.env)"
    fi
    # The release binaries are statically linked, so only the tools we use plus clang's
    # resource directory (freestanding headers) are extracted: ~650 MB instead of ~10 GB.
    log "extracting LLVM $LLVM_VERSION (only the tools Nexora OS uses)"
    rm -rf "$PREFIX/llvm" "$PREFIX/llvm.tmp"
    mkdir -p "$PREFIX/llvm.tmp"
    top="${LLVM_TARBALL%.tar.xz}"
    members=("$top/lib/clang")
    for t in clang clang++ "clang-$LLVM_MAJOR" lld ld.lld lld-link llvm-ar llvm-ranlib \
             llvm-objcopy llvm-strip llvm-nm llvm-readobj llvm-readelf llvm-objdump \
             llvm-symbolizer llvm-addr2line llvm-size; do
        members+=("$top/bin/$t")
    done
    tar -xJf "$tarball" -C "$PREFIX/llvm.tmp" "${members[@]}"
    mv "$PREFIX/llvm.tmp/$top" "$PREFIX/llvm"
    rmdir "$PREFIX/llvm.tmp"
    rm -f "$tarball" "$tarball.done"
    "$PREFIX/llvm/bin/clang" --version | head -n1
    touch "$llvm_stamp"
else
    log "LLVM $LLVM_VERSION already installed"
fi

# ---------------------------------------------------------------------------------------------
# Rust
# ---------------------------------------------------------------------------------------------
export RUSTUP_HOME="$PREFIX/rustup"
export CARGO_HOME="$PREFIX/cargo"
rust_stamp="$STAMP_DIR/rust-$RUST_VERSION"
if [ ! -f "$rust_stamp" ]; then
    if [ ! -x "$CARGO_HOME/bin/rustup" ]; then
        log "installing rustup"
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o "$PREFIX/rustup-init.sh"
        sh "$PREFIX/rustup-init.sh" -y --no-modify-path --profile minimal --default-toolchain none
        rm -f "$PREFIX/rustup-init.sh"
    fi
    log "installing Rust $RUST_VERSION + $RUST_KERNEL_TARGET"
    "$CARGO_HOME/bin/rustup" toolchain install "$RUST_VERSION" --profile minimal --target "$RUST_KERNEL_TARGET"
    "$CARGO_HOME/bin/rustup" default "$RUST_VERSION"
    touch "$rust_stamp"
else
    log "Rust $RUST_VERSION already installed"
fi

# ---------------------------------------------------------------------------------------------
# Limine
# ---------------------------------------------------------------------------------------------
limine_stamp="$STAMP_DIR/limine-$LIMINE_TAG"
if [ ! -f "$limine_stamp" ]; then
    log "fetching Limine $LIMINE_TAG"
    rm -rf "$PREFIX/limine"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$LIMINE_TAG" \
        https://github.com/limine-bootloader/limine.git "$PREFIX/limine"
    got="$(git -C "$PREFIX/limine" rev-parse HEAD)"
    [ "$got" = "$LIMINE_COMMIT" ] || die "Limine $LIMINE_TAG resolved to $got, expected $LIMINE_COMMIT"
    for f in BOOTX64.EFI limine-uefi-cd.bin; do
        [ -f "$PREFIX/limine/$f" ] || die "Limine checkout is missing $f"
    done
    touch "$limine_stamp"
else
    log "Limine $LIMINE_TAG already installed"
fi

cat > "$PREFIX/env.sh" <<EOF
# Source this file to put the Nexora OS toolchain on PATH.
export HZ_TOOLCHAIN="$PREFIX"
export RUSTUP_HOME="$PREFIX/rustup"
export CARGO_HOME="$PREFIX/cargo"
export PATH="$PREFIX/llvm/bin:$PREFIX/cargo/bin:\$PATH"
EOF

log "done. Toolchain prefix: $PREFIX"
