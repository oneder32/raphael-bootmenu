#!/usr/bin/env python3
"""
Unpack an Android boot image (v0 / "ANDROID!" format).
Extracts the kernel and gzip-compressed ramdisk cpio.

Usage: unpack_bootimg.py <boot.img> <out_dir>
"""
import struct
import sys
import os


def pad(size, page):
    return ((size + page - 1) // page) * page


def unpack(img_path, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    with open(img_path, 'rb') as f:
        data = f.read()

    assert data[0:8] == b'ANDROID!', "Not an Android boot v0 image"

    kernel_size = struct.unpack_from('<I', data, 8)[0]
    ramdisk_size = struct.unpack_from('<I', data, 16)[0]
    second_size = struct.unpack_from('<I', data, 24)[0]
    page_size = struct.unpack_from('<I', data, 36)[0]
    cmdline = data[64:576].split(b'\x00', 1)[0].decode('utf-8', 'replace')

    off = page_size
    kernel = data[off:off + kernel_size]
    off += pad(kernel_size, page_size)

    ramdisk = data[off:off + ramdisk_size]
    off += pad(ramdisk_size, page_size)

    with open(os.path.join(out_dir, 'kernel'), 'wb') as f:
        f.write(kernel)
    with open(os.path.join(out_dir, 'ramdisk.cpio.gz'), 'wb') as f:
        f.write(ramdisk)

    print(f"page_size      = {page_size}")
    print(f"kernel_size    = {kernel_size}")
    print(f"ramdisk_size   = {ramdisk_size}")
    print(f"second_size    = {second_size}")
    print(f"cmdline        = {cmdline}")
    print(f"kernel   -> {os.path.join(out_dir, 'kernel')}")
    print(f"ramdisk  -> {os.path.join(out_dir, 'ramdisk.cpio.gz')}")


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    unpack(sys.argv[1], sys.argv[2])
