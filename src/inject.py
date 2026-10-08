#!/usr/bin/env python3
"""
Inject BootMenu into an extracted recovery ramdisk tree:
  - copy the bootmenu binary and wrapper into /sbin
  - rewrite init.recovery.service.rc so the `recovery` service runs
    /sbin/bootmenu_wrapper.sh instead of the real recovery binary.

The wrapper mounts /data, shows the menu, flashes the chosen images and
finally execs the real recovery binary (/system/bin/recovery), so the
original recovery is untouched on disk and still runs as a fallback.

Usage: inject.py <ramdisk_dir> <bootmenu_binary> <wrapper_script>
"""
import os
import sys
import shutil
import stat

RC_NAME = "init.recovery.service.rc"


def patch_service_rc(rc_path):
    with open(rc_path, 'r', encoding='utf-8', errors='replace') as f:
        text = f.read()

    lines = text.splitlines()
    out = []
    patched = False
    for line in lines:
        stripped = line.lstrip()
        if stripped.startswith("service recovery "):
            indent = line[:len(line) - len(stripped)]
            out.append(f"{indent}service recovery /sbin/bootmenu_wrapper.sh")
            patched = True
        else:
            out.append(line)

    if not patched:
        # No recovery service found; append one (keeps the recovery socket +
        # SELinux label OrangeFox/TWRP expect).
        out += [
            "",
            "# BootMenu: added by inject.py",
            "service recovery /sbin/bootmenu_wrapper.sh",
            "    socket recovery stream 422 system system",
            "    seclabel u:r:recovery:s0",
            "",
        ]

    with open(rc_path, 'w', encoding='utf-8') as f:
        f.write("\n".join(out) + "\n")
    return patched


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    ramdisk_dir, binary, wrapper = sys.argv[1:4]

    sbin = os.path.join(ramdisk_dir, "sbin")
    os.makedirs(sbin, exist_ok=True)

    dst_bin = os.path.join(sbin, "bootmenu")
    dst_wrap = os.path.join(sbin, "bootmenu_wrapper.sh")
    shutil.copyfile(binary, dst_bin)
    shutil.copyfile(wrapper, dst_wrap)
    os.chmod(dst_bin, 0o755)
    os.chmod(dst_wrap, 0o755)
    print(f"installed {dst_bin}")
    print(f"installed {dst_wrap}")

    rc_path = os.path.join(ramdisk_dir, RC_NAME)
    if not os.path.exists(rc_path):
        print(f"ERROR: {RC_NAME} not found in ramdisk root", file=sys.stderr)
        sys.exit(2)
    replaced = patch_service_rc(rc_path)
    print(f"patched {RC_NAME} (recovery service replaced={replaced})")


if __name__ == '__main__':
    main()
