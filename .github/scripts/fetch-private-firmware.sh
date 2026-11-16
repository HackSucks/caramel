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
  F=mrvl/sd8797_uapsta.bin
  ok=0
  for url in \
    "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/$F" \
    "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/$F"; do
    if curl -fL --connect-timeout 20 --max-time 120 --retry 3 --retry-delay 5 \
         -o "$DEST/sd8797_uapsta.bin" "$url"; then ok=1; break; fi
  done
  [ "$ok" = 1 ] || { echo "firmware download failed on all mirrors"; exit 1; }
fi
test -s "$DEST/sd8797_uapsta.bin"
