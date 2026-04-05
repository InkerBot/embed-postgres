#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/out"
TARGETS="${1:-linux-amd64 linux-arm64}"

# Ensure buildx builder
docker buildx inspect embed-pg >/dev/null 2>&1 || \
  docker buildx create --name embed-pg --use

for target in $TARGETS; do
    os="${target%%-*}"
    arch="${target#*-}"

    echo "=== Building ${os}/${arch} ==="

    if [ "$os" = "linux" ]; then
        docker buildx build \
            --builder embed-pg \
            --platform "linux/${arch}" \
            --target output \
            --output "type=local,dest=${OUT_DIR}/${target}" \
            "${SCRIPT_DIR}"

    elif [ "$os" = "darwin" ]; then
        # macOS cross-compilation (always runs on linux/amd64)
        # Requires: sdk/MacOSX*.sdk.tar.xz
        MACOS_SDK_TARBALL="${MACOS_SDK_TARBALL:-MacOSX14.0.sdk.tar.xz}"

        if [ ! -f "${SCRIPT_DIR}/sdk/${MACOS_SDK_TARBALL}" ]; then
            echo "ERROR: sdk/${MACOS_SDK_TARBALL} not found."
            echo "Extract it from Xcode: osxcross/tools/gen_sdk_package.sh"
            exit 1
        fi

        docker buildx build \
            --builder embed-pg \
            --platform linux/amd64 \
            --build-arg TARGET_OS=darwin \
            --build-arg DARWIN_ARCH="${arch}" \
            --build-arg MACOS_SDK_TARBALL="${MACOS_SDK_TARBALL}" \
            --target output \
            --output "type=local,dest=${OUT_DIR}/${target}" \
            "${SCRIPT_DIR}"
    fi

    echo "=== Done: ${os}/${arch} ==="
done

echo ""
echo "Build artifacts:"
find "${OUT_DIR}" -name '*.a' -o -name '*.zst' 2>/dev/null | sort