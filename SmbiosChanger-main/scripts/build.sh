#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_PARENT="$(dirname "$PROJECT_ROOT")"
PROJECT_NAME="$(basename "$PROJECT_ROOT")"

if [[ -z "${WORKSPACE:-}" ]]; then
  echo "ERROR: WORKSPACE is not set."
  echo "Example: export WORKSPACE=/home/andromeda/edk2"
  exit 1
fi

export EDK_TOOLS_PATH="${EDK_TOOLS_PATH:-$WORKSPACE/BaseTools}"

export PACKAGES_PATH="$WORKSPACE:$PROJECT_PARENT"

if [[ ":$PATH:" != *":$WORKSPACE/BaseTools/BinWrappers/PosixLike:"* ]]; then
  export PATH="$WORKSPACE/BaseTools/BinWrappers/PosixLike:$PATH"
fi
if [[ ":$PATH:" != *":$WORKSPACE/BaseTools/BinWrappers/Posix:"* ]]; then
  export PATH="$WORKSPACE/BaseTools/BinWrappers/Posix:$PATH"
fi

ARCH="${1:-X64}"
TARGET="${2:-RELEASE}"
TOOLCHAIN="${3:-GCC}"

cd "$WORKSPACE"
build -a "$ARCH" -b "$TARGET" -t "$TOOLCHAIN" -p "$PROJECT_NAME/SmbiosSpooferV2.dsc"
