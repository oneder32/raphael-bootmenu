#!/usr/bin/env bash
#
# BootMenu for Redmi K20 Pro (raphael) - one-click build
#
# Builds a boot.img (from an OrangeFox/TWRP recovery base) that shows a
# graphical + touch multi-boot menu on startup.
#
# Usage:
#   ./build.sh <base_recovery.img> [output_boot.img]
#
# Requirements (Debian/Ubuntu):
#   sudo apt-get install gcc-aarch64-linux-gnu cpio python3
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$ROOT/src"
WORK="$ROOT/build"
OUT="$ROOT/out"

BASE_IMG="${1:-}"
FINAL_IMG="${2:-$OUT/boot.img}"

if [ -z "$BASE_IMG" ] || [ ! -f "$BASE_IMG" ]; then
    echo "Usage: $0 <base_recovery.img> [output_boot.img]"
    echo "  base_recovery.img  an OrangeFox/TWRP recovery image for raphael"
    exit 1
fi

CROSS=aarch64-linux-gnu-gcc
if ! command -v "$CROSS" >/dev/null 2>&1; then
    echo "ERROR: $CROSS not found."
    echo "Install it with: sudo apt-get install gcc-aarch64-linux-gnu"
    exit 1
fi
if ! command -v cpio >/dev/null 2>&1; then
    echo "ERROR: cpio not found. Install with: sudo apt-get install cpio"
    exit 1
fi

rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"

# 1. Compile the static aarch64 bootmenu binary
echo "==> [1/5] Compiling bootmenu"
"$CROSS" -static -O2 -s -I "$SRC" -o "$WORK/bootmenu" "$SRC/bootmenu.c" "$SRC/stb_impl.c" -lm

# 2. Unpack the base recovery image (kernel + ramdisk)
echo "==> [2/5] Unpacking base image"
python3 "$SRC/unpack_bootimg.py" "$BASE_IMG" "$WORK/unpacked"

# 3. Extract the ramdisk cpio
echo "==> [3/5] Extracting ramdisk"
mkdir -p "$WORK/ramdisk"
( cd "$WORK/ramdisk" && zcat "$WORK/unpacked/ramdisk.cpio.gz" | cpio -idmu --quiet )

# 4. Inject bootmenu + wrapper and patch the recovery service
echo "==> [4/5] Injecting BootMenu into ramdisk"
cp "$SRC/bootmenu_wrapper.sh" "$WORK/bootmenu_wrapper.sh"
python3 "$SRC/inject.py" "$WORK/ramdisk" "$WORK/bootmenu" "$WORK/bootmenu_wrapper.sh"

# 5. Repack ramdisk (newc + gzip) and rebuild the boot image
echo "==> [5/5] Repacking boot image"
python3 "$SRC/repack_cpio.py" "$WORK/ramdisk" \
    "$WORK/unpacked/ramdisk_new.cpio" "$WORK/unpacked/ramdisk_new.cpio.gz"
python3 "$SRC/pack_bootimg.py" "$BASE_IMG" \
    "$WORK/unpacked/kernel" "$WORK/unpacked/ramdisk_new.cpio.gz" "$FINAL_IMG"

echo ""
echo "Done -> $FINAL_IMG"
echo "Flash it from recovery with: scripts/flash_bootmenu.sh"
