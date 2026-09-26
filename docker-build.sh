#!/bin/sh
# Build nds-bootstrap in the same devkitARM image CI uses (libnds 1.x).
# Usage (from WSL or Linux, repo root): ./docker-build.sh [make target]
set -e
IMAGE=devkitpro/devkitarm:20241104
TARGET=${1:-package-nightly}

docker run --rm -v "$(pwd)":/src -w /src "$IMAGE" sh -ec "
  git config --global safe.directory '*'
  if ! command -v gcc >/dev/null; then
    sed -i '/bullseye-security/d' /etc/apt/sources.list
    apt-get update -qq
    apt-get install -y -qq gcc >/dev/null
  fi
  gcc lzss.c -o /usr/local/bin/lzss
  make $TARGET
"
