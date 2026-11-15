#!/usr/bin/env bash
set -euo pipefail
FW_REPO_PAT="${1:-}"
KERNEL_SOURCE_DIR="$2"
DEST="${KERNEL_SOURCE_DIR}/extra_firmware/mrvl"
mkdir -p "$DEST"
if [ -n "$FW_REPO_PAT" ]; then
  git clone --depth=1 "https://x-access-token:${FW_REPO_PAT}@github.com/sony-jaguar-devs/firmware.git" /tmp/jaguar-fw
  cp /tmp/jaguar-fw/sd8797_uapsta.bin "$DEST/sd8797_uapsta.bin"
  rm -rf /tmp/jaguar-fw
else
  curl -fL -o "$DEST/sd8797_uapsta.bin" \
    "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mrvl/sd8797_uapsta.bin"
fi
test -s "$DEST/sd8797_uapsta.bin"
