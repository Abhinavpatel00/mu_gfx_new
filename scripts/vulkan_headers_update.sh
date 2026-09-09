#!/usr/bin/env bash

set -euo pipefail

VULKAN_REPO="https://github.com/KhronosGroup/Vulkan-Headers.git"
DEST_DIR="external/vulkan"
TMP_DIR="$(mktemp -d)"

cleanup()
{
    rm -rf "$TMP_DIR"
}

trap cleanup EXIT

command -v git >/dev/null 2>&1 || {
    echo "error: git is required"
    exit 1
}

command -v find >/dev/null 2>&1 || {
    echo "error: find is required"
    exit 1
}

echo "==> Cloning Vulkan-Headers..."

git clone \
    --depth 1 \
    "$VULKAN_REPO" \
    "$TMP_DIR/Vulkan-Headers"

SOURCE_DIR="$TMP_DIR/Vulkan-Headers"
SOURCE_INCLUDE="$SOURCE_DIR/include"

if [[ ! -d "$SOURCE_INCLUDE/vulkan" ]]; then
    echo "error: Vulkan include/vulkan directory not found"
    exit 1
fi

echo "==> Removing everything except include/..."

find "$SOURCE_DIR" \
    -mindepth 1 \
    -maxdepth 1 \
    ! -name "include" \
    -exec rm -rf {} +

echo "==> Removing C++ headers..."

find "$SOURCE_INCLUDE" \
    -type f \
    -name '*.hpp' \
    -print \
    -delete

find "$SOURCE_INCLUDE" \
    -type f \
    \( \
        -name '*.cpp' \
        -o -name '*.cc' \
        -o -name '*.cxx' \
    \) \
    -print \
    -delete

echo "==> Preparing $DEST_DIR..."

rm -rf "$DEST_DIR"
mkdir -p "$DEST_DIR"

echo "==> Installing include/..."

mv "$SOURCE_INCLUDE" "$DEST_DIR/"

if [[ ! -d "$DEST_DIR/include/vulkan" ]]; then
    echo "error: installation failed"
    exit 1
fi

if find "$DEST_DIR/include" -type f -name '*.hpp' | grep -q .; then
    echo "error: .hpp files still exist"
    exit 1
fi

if [[ -d "$DEST_DIR/.git" ]]; then
    echo "error: .git directory exists"
    exit 1
fi

echo
echo "Vulkan headers installed:"
echo "$DEST_DIR/include/vulkan"
echo
echo "Header count:"
find "$DEST_DIR/include" -type f | wc -l
echo
echo "Done."
