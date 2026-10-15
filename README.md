# BootMenu for Redmi K20 Pro (raphael)

A graphical, touch-enabled multi-boot selector that runs from the **boot**
partition of the Redmi K20 Pro (codename **raphael**, Snapdragon 855 / msmnile).

On every normal boot it shows a menu of the ROMs you configured. Pick one and it
writes that ROM's `boot.img` to the **recovery** partition, its `dtbo.img` to
the **dtbo** partition, then reboots into recovery — which now boots the ROM you
chose. The BootMenu itself stays untouched in the boot partition, so the menu
keeps working and you can switch ROMs on every reboot.

Title bar: `BootMenu v0.8  by半杯苏打`

**v0.8 changes**

- **Back key exits the menu** into OrangeFox/TWRP recovery (the wrapper then
  execs the real recovery).
- **Unlimited ROM entries** — the 10-entry cap is gone; long lists scroll
  automatically with `^ more` / `v more` indicators.
- Version bumped to v0.8; the title/署名 is unchanged.

**v0.7 features (still present)**

- **GRUB-style theming** — a background image (PNG/JPG/BMP) plus custom
  `#RRGGBB` colors via a `theme.conf`.
- **Recovery entry** — boot straight into OrangeFox/TWRP from the list.
- **Power actions** — *Fastboot / Bootloader*, *EDL (9008)*, *Reboot*,
  *Poweroff*.
- **Countdown + default entry** — auto-boot a chosen ROM after N seconds;
  set `timeout=0` to disable.
- **Battery indicator** — live percentage (and charging state) in the header.

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
        │ read /data/setboot/file.txt (unlim) │
        │ ROMs + Recovery / power actions     │
        │ volume ± navigate / touch / power   │
        │ optional countdown → default ROM    │
        │ Back → exit to OrangeFox recovery   │
        └─────────────────────────────────────┘
   │                          │
 select a ROM            choose an action
   │                          │
   ▼                          ▼
dd ROM boot.img ─►recovery   Recovery → reboot recovery
dd ROM dtbo.img ─►dtbo       Fastboot/EDL → BCB + reboot
misc = boot-recovery         Reboot / Poweroff → kernel reboot/poweroff
reboot ─► recovery partition (now the chosen ROM) ─► ROM boots
```

BootMenu lives in **boot**; the selected ROM is launched through the
**recovery** partition, so the menu is never overwritten by a ROM.

## Controls

| Input            | Action                          |
|------------------|---------------------------------|
| Volume + / -     | move selection up / down (scrolls when the list is long) |
| Power key        | confirm selection               |
| Touch an entry   | select that entry directly      |
| Any key / touch  | cancel a running countdown      |
| Back key         | **exit the menu into OrangeFox/TWRP recovery** |

The **Back key exits** the menu: the wrapper then execs the real
OrangeFox recovery. You can also enter recovery via the **Recovery** entry in
the *引导 / 电源* (boot / power) section. The menu always shows the power
actions even if no ROM is configured; only when no display can be opened does
it exit and start the real OrangeFox recovery.

## Configuration

`/data/setboot/file.txt` contains ROM lines and optional settings.

**ROM lines** — one per line, **no limit** (long lists scroll automatically):

```
# comments start with #
Display Name|/path/to/boot.img|/path/to/dtbo.img
```

- Paths are relative to `/data`. A leading `/` that is not `/data/` gets
  `/data` prepended automatically, so `/setboot/los21/boot.img` works.
- Names are free text (Chinese and other Unicode supported via the built-in
  glyphs plus an optional external font).

**Settings** — `key=value` (or `key: value`):

| Key | Meaning |
|-----|---------|
| `timeout=10` | countdown seconds before auto-booting the default ROM; any input cancels it. **`0` disables the countdown** (default). |
| `default=LineageOS 21` | default ROM by name, or by 1-based number (`default=2`). Falls back to the first ROM. Marked with a **默认** badge. |
| `theme=/setboot/theme.conf` | GRUB-style theme file. If omitted, `/data/setboot/theme.conf` is tried automatically. |

Example:

```
MIUI 14 Stable|/setboot/miui14/boot.img|/setboot/miui14/dtbo.img
LineageOS 21|/setboot/los21/boot.img|/setboot/los21/dtbo.img
Evolution X|/setboot/evox/boot.img|/setboot/evox/dtbo.img
timeout=10
default=LineageOS 21
```

Layout on `/data`:

```
/data/setboot/
├── file.txt          # config
├── theme.conf        # optional, GRUB-style theme (colors + background)
├── bg.png            # optional, theme background (PNG/JPG/BMP)
├── font.hex          # optional, GNU Unifont .hex for extra glyphs
├── miui14/{boot.img,dtbo.img}
└── los21/{boot.img,dtbo.img}
```

Templates: [`assets/file.txt.example`](assets/file.txt.example) and
[`assets/theme.conf.example`](assets/theme.conf.example).

### GRUB-style theme (`theme.conf`)

A small, GRUB-inspired subset: `key: value` / `key=value`, `#` comments. Colors
are `#RRGGBB`; the background is scaled (center-crop, cover) to the panel.

```
desktop-image: "/setboot/bg.png"
title-color:  "#ffd700"
text-color:   "#eeeeee"
dim-color:    "#9aa4b2"
panel-color:  "#0f3460"
border-color: "#e94560"
selected-bg:  "#e94560"
selected-fg:  "#ffffff"
accent-color: "#22d3ee"
bg-top:       "#1a1a2e"   # fallback gradient when no image is set
bg-bottom:    "#16213e"
```

Image decoding uses the public-domain **stb_image** single-header library
(`src/stb_image.h`, implemented in `src/stb_impl.c`); no external image
libraries are needed.

### Power actions

The *引导 / 电源* section always provides **Recovery**, **Fastboot /
Bootloader**, **EDL 模式 (9008)**, **Reboot 重启**, and **Poweroff 关机**.
Fastboot uses the BCB (`bootonce-bootloader`) plus the `bootloader` reboot
reason; EDL tries the `edl`/`emergency`/`9008` reboot reasons and the Qualcomm
`android_usb boot_mode` node, falling back to fastboot if the kernel/firmware
does not support it. **EDL is high-risk** (9008 deep-flash mode).

### Unicode / Chinese font

The binary has a built-in 8×16 ASCII font plus the 16×16 glyphs used by the UI
(**半杯苏打**, 引导/电源/重启/关机/默认/电量/充电中/模式 etc.). For arbitrary
Unicode menu names, drop a GNU Unifont `.hex` file at
`/data/setboot/font.hex`; both 8×16 and 16×16 glyphs are parsed. Get it from
<https://unifoundry.com/unifont/>.

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
| `src/bootmenu.c` | the menu program (DRM/KMS graphics, evdev input, theming, flashing, reboot) |
| `src/stb_image.h` / `src/stb_impl.c` | public-domain PNG/JPG/BMP decoder (theme background) |
| `src/bootmenu_wrapper.sh` | recovery-service wrapper: mount /data, run menu, exec recovery |
| `src/unpack_bootimg.py` | split a boot v0 image into kernel + ramdisk |
| `src/inject.py` | install binary/wrapper and patch `init.recovery.service.rc` |
| `src/repack_cpio.py` | rebuild the ramdisk as cpio newc + gzip |
| `src/pack_bootimg.py` | rebuild the Android boot v0 image |

Compile the binary on its own with (note the second translation unit):

```bash
aarch64-linux-gnu-gcc -static -O2 -s -I src -o bootmenu \
    src/bootmenu.c src/stb_impl.c -lm
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
│   ├── file.txt.example      # config template
│   └── theme.conf.example    # GRUB-style theme template
├── scripts/
│   └── flash_bootmenu.sh     # recovery-side installer
└── src/
    ├── bootmenu.c            # menu program
    ├── stb_image.h           # PNG/JPG/BMP decoder (header-only)
    ├── stb_impl.c            # stb_image implementation unit
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
