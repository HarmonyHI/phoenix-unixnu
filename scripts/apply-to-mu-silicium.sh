#!/bin/sh
# Recreate the working firmware tree: pristine Mu-Silicium + this repo's overlay and patches.
#
#   ./scripts/apply-to-mu-silicium.sh /path/to/Mu-Silicium
#
# The target must be a clean checkout of Project-Silicium/Mu-Silicium at the
# commit recorded in docs/PORTING-PHOENIX.md. Nothing is deleted; the script
# stops if a patch does not apply.

set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
TARGET=${1:-}

if [ -z "$TARGET" ]; then
    echo "usage: $0 /path/to/Mu-Silicium" >&2
    exit 2
fi
if [ ! -d "$TARGET/Platforms/Xiaomi/phoenixPkg" ]; then
    echo "error: $TARGET does not look like a Mu-Silicium checkout" >&2
    exit 2
fi

echo "==> overlay (new files)"
cp -R "$HERE/overlay/." "$TARGET/"

echo "==> patch 0001: phoenixPkg + UniXNU wiring"
git -C "$TARGET" apply --index "$HERE/patches/0001-mu-silicium-phoenix-port.patch"

echo "==> the payload volume is a build artifact and is not shipped"
echo "    run Resources/Scripts/build-phoenix.sh to regenerate Payload/payload.fat"

echo "==> done. Next:"
echo "    cd $TARGET && ./Resources/Scripts/build-phoenix.sh Mu-phoenix-adt.img"
