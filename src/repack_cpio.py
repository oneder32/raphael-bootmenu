#!/usr/bin/env python3
"""
Repack ramdisk directory into cpio newc format + gzip.
Preserves permissions, symlinks, and directory structure.
"""
import os
import sys
import struct
import gzip
import stat

def create_cpio(src_dir, out_path):
    entries = []
    
    # Walk directory tree
    for root, dirs, files in os.walk(src_dir):
        # Sort for deterministic output
        dirs.sort()
        files.sort()
        
        rel_root = os.path.relpath(root, src_dir)
        if rel_root == '.':
            rel_root = ''
        
        # Add directory entry (except root which is '.')
        if rel_root:
            entries.append(('dir', rel_root, root))
        
        for name in files:
            full = os.path.join(root, name)
            rel = os.path.join(rel_root, name) if rel_root else name
            if os.path.islink(full):
                entries.append(('symlink', rel, full))
            else:
                entries.append(('file', rel, full))
    
    # Sort entries: directories first, then files (like original)
    # Actually, cpio order doesn't matter much, but let's keep it sorted
    entries.sort(key=lambda x: x[1])
    
    with open(out_path, 'wb') as f:
        inode = 1
        for entry_type, name, full_path in entries:
            name_bytes = name.encode('utf-8') + b'\x00'
            namesize = len(name_bytes)
            
            if entry_type == 'dir':
                mode = 0o040755
                filesize = 0
                data = b''
            elif entry_type == 'symlink':
                mode = 0o120777
                target = os.readlink(full_path)
                data = target.encode('utf-8')
                filesize = len(data)
            else:
                st = os.stat(full_path)
                mode = 0o100000 | (st.st_mode & 0o7777)
                with open(full_path, 'rb') as rf:
                    data = rf.read()
                filesize = len(data)
            
            # newc header: 110 bytes total
            # 6 bytes magic "070701" + 13 fields of 8 hex chars each
            header = b'070701' + '{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}'.format(
                inode, mode, 0, 0, 1, 0, filesize,
                0, 0, 0, 0, namesize, 0
            ).encode('ascii')
            
            f.write(header)
            f.write(name_bytes)
            
            # Pad name to 4-byte boundary
            name_pad = (4 - (110 + namesize) % 4) % 4
            f.write(b'\x00' * name_pad)
            
            # Write data
            if filesize > 0:
                f.write(data)
                # Pad data to 4-byte boundary
                data_pad = (4 - filesize % 4) % 4
                f.write(b'\x00' * data_pad)
            
            inode += 1
        
        # Write TRAILER!!! entry
        trailer_name = b'TRAILER!!!\x00'
        trailer_namesize = len(trailer_name)
        header = b'070701' + '{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}'.format(
            0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, trailer_namesize, 0
        ).encode('ascii')
        f.write(header)
        f.write(trailer_name)
        trailer_pad = (4 - (110 + trailer_namesize) % 4) % 4
        f.write(b'\x00' * trailer_pad)
    
    print(f"CPIO created: {out_path} ({os.path.getsize(out_path)} bytes)")
    print(f"Total entries: {len(entries)}")

if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else 'ramdisk'
    out_cpio = sys.argv[2] if len(sys.argv) > 2 else 'ramdisk_new.cpio'
    out_gz = sys.argv[3] if len(sys.argv) > 3 else 'ramdisk_new.cpio.gz'
    
    create_cpio(src, out_cpio)
    
    # Gzip compress
    with open(out_cpio, 'rb') as f_in:
        with gzip.open(out_gz, 'wb', compresslevel=9) as f_out:
            f_out.write(f_in.read())
    
    print(f"GZIpped: {out_gz} ({os.path.getsize(out_gz)} bytes)")
