#!/usr/bin/env python3
"""
Pack Android boot image (v0 format) from kernel + ramdisk.
Based on the original recovery.img header.
"""
import struct
import sys
import os

def pack_boot_image(original_img, kernel_path, ramdisk_path, output_path):
    with open(original_img, 'rb') as f:
        orig = f.read()
    
    # Parse original header
    magic = orig[0:8]
    assert magic == b'ANDROID!', "Not an Android boot image"
    
    kernel_size = struct.unpack_from('<I', orig, 8)[0]
    kernel_addr = struct.unpack_from('<I', orig, 12)[0]
    ramdisk_size = struct.unpack_from('<I', orig, 16)[0]
    ramdisk_addr = struct.unpack_from('<I', orig, 20)[0]
    second_size = struct.unpack_from('<I', orig, 24)[0]
    second_addr = struct.unpack_from('<I', orig, 28)[0]
    tags_addr = struct.unpack_from('<I', orig, 32)[0]
    page_size = struct.unpack_from('<I', orig, 36)[0]
    # bytes 40-47: unused + name
    name = orig[44:60]
    cmdline = orig[60:572]  # 512 bytes cmdline
    # id at 572-604 (32 bytes)
    
    print(f"Original: kernel={kernel_size}, ramdisk={ramdisk_size}, page_size={page_size}")
    print(f"  kernel_addr=0x{kernel_addr:x}, ramdisk_addr=0x{ramdisk_addr:x}")
    
    # Read new kernel and ramdisk
    with open(kernel_path, 'rb') as f:
        kernel = f.read()
    with open(ramdisk_path, 'rb') as f:
        ramdisk = f.read()
    
    new_kernel_size = len(kernel)
    new_ramdisk_size = len(ramdisk)
    
    print(f"New: kernel={new_kernel_size}, ramdisk={new_ramdisk_size}")
    
    # Calculate layout
    def pad_to(size, page):
        return ((size + page - 1) // page) * page
    
    kernel_offset = page_size  # after header
    kernel_padded = pad_to(new_kernel_size, page_size)
    
    ramdisk_offset = kernel_offset + kernel_padded
    ramdisk_padded = pad_to(new_ramdisk_size, page_size)
    
    # No second stage or DT for this image
    total_size = ramdisk_offset + ramdisk_padded
    
    print(f"Layout: header@{0}, kernel@{kernel_offset}, ramdisk@{ramdisk_offset}")
    print(f"Total size: {total_size} bytes ({total_size/1024/1024:.1f} MB)")
    
    # Build header
    header = bytearray(page_size)
    struct.pack_into('<8s', header, 0, b'ANDROID!')
    struct.pack_into('<I', header, 8, new_kernel_size)
    struct.pack_into('<I', header, 12, kernel_addr)
    struct.pack_into('<I', header, 16, new_ramdisk_size)
    struct.pack_into('<I', header, 20, ramdisk_addr)
    struct.pack_into('<I', header, 24, 0)  # second_size
    struct.pack_into('<I', header, 28, second_addr)
    struct.pack_into('<I', header, 32, tags_addr)
    struct.pack_into('<I', header, 36, page_size)
    struct.pack_into('<I', header, 40, 0)  # dt_size / unused
    struct.pack_into('<I', header, 44, 0)  # unused
    # name (16 bytes) at offset 44... wait, let me recalculate
    # Actually the layout is:
    # 0-7: magic
    # 8-11: kernel_size
    # 12-15: kernel_addr
    # 16-19: ramdisk_size
    # 20-23: ramdisk_addr
    # 24-27: second_size
    # 28-31: second_addr
    # 32-35: tags_addr
    # 36-39: page_size
    # 40-43: dt_size (unused in v0)
    # 44-47: unused
    # 48-63: name (16 bytes)
    # 64-575: cmdline (512 bytes)
    # 576-607: id (32 bytes)
    
    # Let me redo this correctly
    header = bytearray(page_size)
    struct.pack_into('<8s', header, 0, b'ANDROID!')
    struct.pack_into('<I', header, 8, new_kernel_size)
    struct.pack_into('<I', header, 12, kernel_addr)
    struct.pack_into('<I', header, 16, new_ramdisk_size)
    struct.pack_into('<I', header, 20, ramdisk_addr)
    struct.pack_into('<I', header, 24, 0)  # second_size = 0
    struct.pack_into('<I', header, 28, second_addr)
    struct.pack_into('<I', header, 32, tags_addr)
    struct.pack_into('<I', header, 36, page_size)
    struct.pack_into('<I', header, 40, 0)  # unused (was dt_size=1 in original, but it's unused in v0)
    struct.pack_into('<I', header, 44, 0)  # unused
    # name at 48 (16 bytes) - copy from original
    header[48:64] = orig[48:64]
    # cmdline at 64 (512 bytes) - copy from original
    header[64:576] = orig[64:576]
    # id at 576 (32 bytes) - will be zero (recalculated by bootloader if needed)
    
    # Write output
    with open(output_path, 'wb') as f:
        f.write(header)
        f.write(kernel)
        f.write(b'\x00' * (kernel_padded - new_kernel_size))
        f.write(ramdisk)
        f.write(b'\x00' * (ramdisk_padded - new_ramdisk_size))
    
    final_size = os.path.getsize(output_path)
    print(f"Output: {output_path} ({final_size} bytes, {final_size/1024/1024:.1f} MB)")
    print(f"Partition limit: 128 MB - {'OK' if final_size <= 128*1024*1024 else 'TOO LARGE!'}")

if __name__ == '__main__':
    original = sys.argv[1] if len(sys.argv) > 1 else '/home/user/.doubao/agent_mode/workspace/.sessions/38445989121762306/attachments/recovery.img'
    kernel = sys.argv[2] if len(sys.argv) > 2 else 'unpacked/kernel'
    ramdisk = sys.argv[3] if len(sys.argv) > 3 else 'unpacked/ramdisk_new.cpio.gz'
    output = sys.argv[4] if len(sys.argv) > 4 else 'out/boot.img'
    
    pack_boot_image(original, kernel, ramdisk, output)
