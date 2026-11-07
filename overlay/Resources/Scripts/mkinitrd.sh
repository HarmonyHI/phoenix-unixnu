#!/bin/bash
#
# Package an already-built phoenix firmware into a fastboot boot image
# (RAM boot only; nothing is written to the device).
#
#   ./Resources/Scripts/mkinitrd.sh [output.img]
#
# The ramdisk half of the boot image is the same payload FAT that is embedded
# in the firmware, so the bootloader path and the firmware fallback stay in
# sync.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

BUILD="${BUILD_DIR:-Build/phoenixPkg/DEBUG_CLANGPDB}"
FD="$BUILD/FV/SILICIUM_UEFI.fd"
OUT="${1:-Mu-phoenix-payload.img}"
FAT="${PAYLOAD_FAT:-Platforms/Xiaomi/phoenixPkg/Payload/payload.fat}"
# The device tree has to ride along after the compressed kernel: the bootloader
# hands its address to the firmware in x0, and without it SEC dies during MMU
# setup (symptom: a lone "splitting block entry with MMU disabled" line, then a
# reset into Android). Boot images built before 2026-11-07 01:14 carried it.
DTB="${DTB:-$ROOT/phoenix-initrd.dtb}"

for f in "$FD" "$FAT"; do
  if [ ! -f "$f" ]; then
    echo "error: $f not found" >&2
    exit 1
  fi
done

cat BootShim/BootShim.bin "$FD" > /tmp/phoenix-cat.bin
python3 - <<'PY'
import gzip
data = open('/tmp/phoenix-cat.bin', 'rb').read()
packed = gzip.compress(data, 9, mtime=0)
open('/tmp/phoenix-kernel.bin', 'wb').write(packed)
print('kernel: %d bytes raw, %d gzipped' % (len(data), len(packed)))
PY

if [ -f "$DTB" ]; then
  cat /tmp/phoenix-kernel.bin "$DTB" > /tmp/phoenix-kernel-dtb.bin
  echo "dtb: $(stat -f '%z' "$DTB" 2>/dev/null || stat -c '%s' "$DTB") bytes appended ($(basename "$DTB"))"
  mv /tmp/phoenix-kernel-dtb.bin /tmp/phoenix-kernel.bin
else
  echo "error: $DTB not found; refusing to build an image the firmware cannot use" >&2
  exit 1
fi

python3 Resources/Scripts/mkbootimg.py \
  --kernel /tmp/phoenix-kernel.bin \
  --ramdisk "$FAT" \
  --header_version 1 \
  --pagesize 2048 \
  --base 0x80000000 \
  --kernel_offset 0x00008000 \
  --ramdisk_offset 0x0AB00000 \
  --tags_offset 0x00000100 \
  -o "$OUT"

ls -la "$OUT"
