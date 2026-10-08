# BootMenu for Redmi K20 Pro (raphael)

A graphical, touch-enabled multi-boot selector that runs from the **boot**
partition of the Redmi K20 Pro (codename **raphael**, Snapdragon 855 / msmnile).

On every normal boot it shows a menu of the ROMs you configured. Pick one and it
writes that ROM's `boot.img` to the **recovery** partition, its `dtbo.img` to
the **dtbo** partition, then reboots into recovery — which now boots the ROM you
chose. The BootMenu itself stays untouched in the boot partition, so the menu
keeps working and you can switch ROMs on every reboot.

Title bar: `BootMenu v0.6  by半杯苏打`

> ⚠️ Flashing boot/recovery images can brick a device if used incorrectly.
> Know how to recover with fastboot before using this. Tested only on
> raphael. Use at your own risk.

## How it works

```
power on
   │
   ▼
boot partition = BootMenu (OrangeFox kernel + patched ramdisk)
   │
   ▼
recovery service replaced by /sbin/bootmenu_wrapper.sh
   │  mounts /data, runs /sbin/bootmenu
   ▼
        ┌──────────── BootMenu UI ────────────┐
        │ read /data/setboot/file.txt (≤10)   │
        │ volume ± navigate / touch / power   │
        └─────────────────────────────────────┘
   │                          │
 select a ROM              press Back
   │                          │
   ▼                          ▼
dd ROM boot.img ─►recovery   exec /system/bin/recovery
dd ROM dtbo.img ─►dtbo       (normal OrangeFox recovery)
misc = boot-recovery
reboot ─► recovery partition (now the chosen ROM) ─► ROM boots
```

BootMenu lives in **boot**; the selected ROM is launched through the
**recovery** partition, so the menu is never overwritten by a ROM.

## Controls

| Input            | Action                          |
|------------------|---------------------------------|
| Volume + / -     | move selection up / down        |
| Power key        | confirm selection               |
| Touch an entry   | select that entry directly      |
| Back key         | skip the menu, enter recovery   |

If `/data/setboot/file.txt` is missing or empty, or no display can be opened,
the menu exits and the real OrangeFox recovery starts normally.

## Configuration

`/data/setboot/file.txt`, one entry per line, **up to 10 entries**:

```
# comments start with #
Display Name|/path/to/boot.img|/path/to/dtbo.img
```

- Paths are relative to `/data`. A leading `/` that is not `/data/` gets
  `/data` prepended automatically, so `/setboot/los21/boot.img` works.
- Names are free text (Chinese and other Unicode supported via the built-in
  glyphs plus an optional external font).

Example:

```
MIUI 14 Stable|/setboot/miui14/boot.img|/setboot/miui14/dtbo.img
LineageOS 21|/setboot/los21/boot.img|/setboot/los21/dtbo.img
Evolution X|/setboot/evox/boot.img|/setboot/evox/dtbo.img
```

Layout on `/data`:

```
/data/setboot/
├── file.txt          # config
├── font.hex          # optional, GNU Unifont .hex for extra glyphs
├── miui14/{boot.img,dtbo.img}
└── los21/{boot.img,dtbo.img}
```

A template is provided in [`assets/file.txt.example`](assets/file.txt.example).

### Unicode / Chinese font

The binary has a built-in 8×16 ASCII font plus the 16×16 glyphs used in the
title (**半杯苏打**). For arbitrary Unicode menu names, drop a GNU Unifont
`.hex` file at `/data/setboot/font.hex`; both 8×16 and 16×16 glyphs are parsed.
Get it from <https://unifoundry.com/unifont/>.

## Build

You need an aarch64 cross toolchain, `cpio` and Python 3:

```bash
sudo apt-get install gcc-aarch64-linux-gnu cpio python3
```

Then build from an OrangeFox (or TWRP) recovery image for raphael:

```bash
./build.sh /path/to/OrangeFox_raphael_recovery.img out/boot.img
```

The script compiles a static `bootmenu` binary, unpacks the base image, injects
the binary + wrapper into the ramdisk, rewrites the `recovery` service, and
repacks an Android boot v0 image (~37 MB, well within the 128 MB partition).

### Manual / step by step

The individual stages live in [`src/`](src/):

| Script | Purpose |
|--------|---------|
| `src/bootmenu.c` | the menu program (DRM/KMS graphics, evdev input, flashing) |
| `src/bootmenu_wrapper.sh` | recovery-service wrapper: mount /data, run menu, exec recovery |
| `src/unpack_bootimg.py` | split a boot v0 image into kernel + ramdisk |
| `src/inject.py` | install binary/wrapper and patch `init.recovery.service.rc` |
| `src/repack_cpio.py` | rebuild the ramdisk as cpio newc + gzip |
| `src/pack_bootimg.py` | rebuild the Android boot v0 image |

Compile the binary on its own with:

```bash
aarch64-linux-gnu-gcc -static -O2 -s -o bootmenu src/bootmenu.c
```

## Install

1. Boot into OrangeFox/TWRP recovery.
2. Copy the built `boot.img` and [`scripts/flash_bootmenu.sh`](scripts/flash_bootmenu.sh)
   to the device (e.g. `/sdcard/bootmenu/`).
3. Run the installer from a recovery terminal / `adb shell`:
   ```sh
   sh /sdcard/bootmenu/flash_bootmenu.sh
   ```
   It backs up the current boot partition, flashes BootMenu to **boot**, and
   creates `/data/setboot/`.
4. Put your ROM images and `file.txt` in `/data/setboot/` and reboot.

Manual equivalent:

```sh
dd if=/sdcard/bootmenu/boot.img of=/dev/block/bootdevice/by-name/boot
mkdir -p /data/setboot
```

## Display notes (raphael)

The panel is a Samsung **command-mode** DSI command-mode panel
(`samsung,ea8076`, 1080×2340) driven by `msm_drm`; there is **no `/dev/fb0`**.
Getting pixels on screen required, in order:

1. open `/dev/dri/card0` and take master (`DRM_IOCTL_SET_MASTER`);
2. pick a valid CRTC from `possible_crtcs` and the connected internal connector
   (ignore the external 4096×2160 connector);
3. create dumb buffers with `DRM_FORMAT_XBGR8888` (fallback `XRGB8888`);
4. **disable the CRTC (`fb_id=0`), wait, then re-enable it** to cleanly take
   over the bootloader's continuous-splash framebuffer;
5. turn the connector DPMS on and set backlight through
   `/sys/class/backlight/panel0-backlight/brightness`;
6. render into a back buffer and present with atomic-ish double-buffered
   `DRM_IOCTL_MODE_PAGE_FLIP` (waiting for the flip event); a plain
   `DIRTYFB` ioctl does **not** work on this panel.

Input devices observed: `qpnp_pon` (power), `gpio-keys` (volume),
`goodix_ts`/`uinput-goodix` (touch).

## Project layout

```
.
├── build.sh                  # one-click build
├── assets/
│   └── file.txt.example      # config template
├── scripts/
│   └── flash_bootmenu.sh     # recovery-side installer
└── src/
    ├── bootmenu.c            # menu program
    ├── bootmenu_wrapper.sh   # recovery-service wrapper
    ├── unpack_bootimg.py
    ├── inject.py
    ├── repack_cpio.py
    └── pack_bootimg.py
```

## License

GPL-2.0 (it links against and reuses the Linux/DRM userspace ABI, and is
designed to be combined with GPL recovery images). See [`LICENSE`](LICENSE).

The base kernel/ramdisk comes from your own OrangeFox/TWRP image and is **not**
redistributed here.
