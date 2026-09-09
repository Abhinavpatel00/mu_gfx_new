#!/usr/bin/env bash

# ============================================================================
# Vendor Vulkan C Headers
# ============================================================================
#
# Purpose:
#
#   Clone the official Vulkan-Headers repository into a temporary directory,
#   extract its include tree, remove C++ (.hpp) headers, and leave only the
#   headers needed by the C side of the project.
#
#
#   SOURCE
#
#       Vulkan-Headers repository
#              |
#              v
#       ┌──────────────────────┐
#       │ temporary git clone  │
#       └──────────┬───────────┘
#                  |
#                  v
#       ┌──────────────────────┐
#       │ include/vulkan/       │
#       │                      │
#       │ *.h                  │
#       │ *.hpp                │
#       │ platform headers     │
#       └──────────┬───────────┘
#                  |
#                  | remove *.hpp
#                  v
#       ┌──────────────────────┐
#       │ external/vulkan/     │
#       │   include/           │
#       │     vulkan/          │
#       │       *.h             │
#       └──────────────────────┘
#
#
# The Git repository itself is NOT copied into the project.
#
# Result:
#
#       external/
#       └── vulkan/
#           └── include/
#               └── vulkan/
#                   ├── vulkan.h
#                   ├── vulkan_core.h
#                   ├── vulkan_beta.h
#                   ├── vk_platform.h
#                   └── ...
#
# ============================================================================

set -euo pipefail


# ----------------------------------------------------------------------------
# Configuration
# ----------------------------------------------------------------------------

VULKAN_REPO="https://github.com/KhronosGroup/Vulkan-Headers.git"

# Where the vendored headers will live.
DEST_DIR="external/vulkan"

# Temporary directory used for cloning.
TMP_DIR="$(mktemp -d)"


# ----------------------------------------------------------------------------
# Cleanup
# ----------------------------------------------------------------------------
#
# Even if the script fails halfway through, remove the temporary clone.
#
# This prevents leaving random Git repositories in /tmp because humans have
# apparently decided that cleanup is an optional feature.
# ----------------------------------------------------------------------------

cleanup()
{
    rm -rf "$TMP_DIR"
}

trap cleanup EXIT


# ----------------------------------------------------------------------------
# Verify dependencies
# ----------------------------------------------------------------------------

command -v git >/dev/null 2>&1 || {
    echo "error: git is required"
    exit 1
}

command -v rsync >/dev/null 2>&1 || {
    echo "error: rsync is required"
    exit 1
}


# ----------------------------------------------------------------------------
# Clone Vulkan-Headers
# ----------------------------------------------------------------------------

echo "==> Cloning Vulkan-Headers..."

git clone \
    --depth 1 \
    "$VULKAN_REPO" \
    "$TMP_DIR/Vulkan-Headers"


# ----------------------------------------------------------------------------
# Verify source layout
# ----------------------------------------------------------------------------

SOURCE_INCLUDE="$TMP_DIR/Vulkan-Headers/include"

if [[ ! -d "$SOURCE_INCLUDE/vulkan" ]]; then
    echo "error: Vulkan include directory not found:"
    echo "       $SOURCE_INCLUDE/vulkan"
    exit 1
fi


# ----------------------------------------------------------------------------
# Prepare destination
# ----------------------------------------------------------------------------
#
# This script owns external/vulkan.
#
# If you don't want destructive replacement, remove this block and use rsync
# without --delete.
# ----------------------------------------------------------------------------

echo "==> Preparing $DEST_DIR..."

rm -rf "$DEST_DIR"

mkdir -p "$DEST_DIR"


# ----------------------------------------------------------------------------
# Copy the complete include tree
# ----------------------------------------------------------------------------
#
# We copy the entire include directory first.
#
# Why?
#
# Because Vulkan has platform-specific headers and directory structure that
# should not be reconstructed manually.
#
#
#       source
#          |
#          v
#       include/
#          |
#          +── vulkan/
#          |    ├── vulkan.h
#          |    ├── vulkan_core.h
#          |    ├── vk_platform.h
#          |    └── ...
#          |
#          v
#       external/vulkan/include/
#
# ----------------------------------------------------------------------------

echo "==> Copying Vulkan headers..."

rsync -a \
    "$SOURCE_INCLUDE/" \
    "$DEST_DIR/include/"


# ----------------------------------------------------------------------------
# Remove C++ headers
# ----------------------------------------------------------------------------
#
# The Vulkan-Headers repository can contain C++ headers such as:
#
#       vulkan.hpp
#
# Your project wants the C interface only.
#
# Find every *.hpp recursively and remove it.
#
# ----------------------------------------------------------------------------

echo "==> Removing C++ headers..."

find "$DEST_DIR/include" \
    -type f \
    -name '*.hpp' \
    -print \
    -delete


# ----------------------------------------------------------------------------
# Remove any C++ source files if they happen to exist
# ----------------------------------------------------------------------------
#
# Normally the include tree should not contain these, but keeping the vendor
# directory strictly C-facing is cheap insurance.
# ----------------------------------------------------------------------------

find "$DEST_DIR/include" \
    -type f \
    \( \
        -name '*.cpp' \
        -o -name '*.cc' \
        -o -name '*.cxx' \
    \) \
    -print \
    -delete


# ----------------------------------------------------------------------------
# Report result
# ----------------------------------------------------------------------------

echo
echo "==> Vulkan headers installed:"
echo "    $DEST_DIR/include"
echo

echo "==> C++ headers remaining:"

if find "$DEST_DIR/include" -type f -name '*.hpp' | grep -q .; then
    echo "error: .hpp files still exist"
    exit 1
else
    echo "    none"
fi

echo
echo "==> Header count:"
find "$DEST_DIR/include" -type f | wc -l

echo
echo "==> Done."
