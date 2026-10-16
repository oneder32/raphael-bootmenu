/*
 * BootMenu - Android Multi-Boot Boot Menu V0.7
 * For Redmi K20 Pro (raphael) / Qualcomm msmnile
 *
 * Features:
 *   - DRM/framebuffer graphical menu with color UI (double-buffered page flip)
 *   - Volume +/- navigation, Power/touch to select
 *   - Touch screen support (tap to select)
 *   - Reads config from /data/setboot/file.txt
 *   - Selecting a ROM flashes boot.img->recovery, dtbo.img->dtbo, reboots recovery
 *   - Up to 10 boot entries, custom names
 *   - GRUB-style theming: theme.conf (colors) + desktop background image (PNG/JPG)
 *   - Built-in action entries: Recovery / Fastboot / EDL(9008) / Reboot / Poweroff
 *   - Countdown auto-boot with configurable default entry (timeout=0 disables)
 *   - Battery level indicator
 *   - Multi-language font (built-in CJK glyphs + optional /data/setboot/font.hex)
 *   - UTF-8 text rendering
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <stdarg.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <stdint.h>
#include "stb_image.h"

#ifndef DRM_MODE_CONNECTED
#define DRM_MODE_CONNECTED 1
#endif

/* ============ Debug logging ============ */
static FILE *log_fp = NULL;
static void bm_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (!log_fp) {
        log_fp = fopen("/tmp/bootmenu.log", "w");
    }
    if (log_fp) {
        va_start(ap, fmt);
        vfprintf(log_fp, fmt, ap);
        va_end(ap);
        fflush(log_fp);
    }
}

/* Try to set SELinux permissive */
static void set_selinux_permissive(void) {
    int fd = open("/sys/fs/selinux/enforce", O_WRONLY);
    if (fd >= 0) {
        write(fd, "0", 1);
        close(fd);
    }
}

/* ============ Constants ============ */
#define MAX_ACTIONS     5
#define MAX_NAME_LEN    128
#define MAX_PATH_LEN    256
#define CONFIG_PATH     "/data/setboot/file.txt"
#define FONT_PATH       "/data/setboot/font.hex"
#define THEME_AUTO_PATH "/data/setboot/theme.conf"
#define RECOVERY_DEV    "/dev/block/bootdevice/by-name/recovery"
#define DTBO_DEV        "/dev/block/bootdevice/by-name/dtbo"
#define MISC_DEV        "/dev/block/bootdevice/by-name/misc"
#define BOOTMENU_VERSION "0.8"

/* Menu item types */
enum { ITEM_ROM = 0, ITEM_RECOVERY, ITEM_FASTBOOT, ITEM_EDL, ITEM_REBOOT, ITEM_POWEROFF };

/* ============ Framebuffer ============ */
static int fb_fd = -1;
static struct fb_var_screeninfo fb_vinfo;
static struct fb_fix_screeninfo fb_finfo;
static unsigned char *fb_mem = NULL;
static int fb_size = 0;
static int fb_bpp = 32;
static int fb_width = 0;
static int fb_height = 0;
static int fb_line_length = 0;
static int display_is_drm = 0; /* 0=fbdev, 1=DRM */

/* ============ DRM display (double-buffered page flip) ============ */
static int drm_fd = -1;
#define DRM_BUFFERS 2
static uint32_t drm_fb_ids[DRM_BUFFERS] = {0, 0};
static uint32_t drm_bo_handles[DRM_BUFFERS] = {0, 0};
static unsigned char *drm_bufs[DRM_BUFFERS] = {NULL, NULL};
static int drm_front = 0; /* currently displayed buffer */
static uint32_t drm_crtc_id = 0;
static uint32_t drm_connector_id = 0;
static struct drm_mode_modeinfo drm_mode;

static int drm_init(void) {
    /* Try to open DRM card */
    drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) drm_fd = open("/dev/dri/card1", O_RDWR);
    if (drm_fd < 0) {
        bm_log("[BootMenu] DRM: Cannot open /dev/dri/card0\n");
        return -1;
    }
    bm_log("[BootMenu] DRM: opened card\n");

    /* Explicitly acquire DRM master (needed for modeset/pageflip) */
    int master_ret = ioctl(drm_fd, DRM_IOCTL_SET_MASTER, 0);
    bm_log("[BootMenu] DRM: SET_MASTER ret=%d errno=%d(%s)\n",
           master_ret, errno, master_ret < 0 ? strerror(errno) : "ok");

    /* Get resources */
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        bm_log("[BootMenu] DRM: GETRESOURCES failed: %s\n", strerror(errno));
        return -1;
    }
    bm_log("[BootMenu] DRM: connectors=%d, crtcs=%d, encoders=%d\n",
           res.count_connectors, res.count_crtcs, res.count_encoders);

    if (res.count_connectors == 0) {
        bm_log("[BootMenu] DRM: No connectors\n");
        return -1;
    }

    /* Allocate connector IDs */
    uint32_t *conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
    uint32_t *crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
    uint32_t *enc_ids = calloc(res.count_encoders, sizeof(uint32_t));
    if (!conn_ids || !crtc_ids || !enc_ids) {
        free(conn_ids); free(crtc_ids); free(enc_ids);
        return -1;
    }
    res.connector_id_ptr = (uint64_t)(unsigned long)conn_ids;
    res.crtc_id_ptr = (uint64_t)(unsigned long)crtc_ids;
    res.encoder_id_ptr = (uint64_t)(unsigned long)enc_ids;
    ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res);

    int found = 0;
    for (int ci = 0; ci < res.count_connectors && !found; ci++) {
        struct drm_mode_get_connector conn;
        memset(&conn, 0, sizeof(conn));
        conn.connector_id = conn_ids[ci];
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        if (conn.connection != DRM_MODE_CONNECTED) continue;

        bm_log("[BootMenu] DRM: connector %d connected, modes=%d, encoder=%d\n",
               ci, conn.count_modes, conn.encoder_id);

        if (conn.count_modes == 0) continue;

        /* Get modes */
        struct drm_mode_modeinfo *modes = calloc(conn.count_modes, sizeof(struct drm_mode_modeinfo));
        if (!modes) continue;
        conn.modes_ptr = (uint64_t)(unsigned long)modes;
        conn.count_props = 0;
        conn.count_encoders = 0;
        ioctl(drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);

        /* Use first mode (preferred/native) */
        memcpy(&drm_mode, &modes[0], sizeof(drm_mode));
        free(modes);

        fb_width = drm_mode.hdisplay;
        fb_height = drm_mode.vdisplay;
        fb_bpp = 32;

        bm_log("[BootMenu] DRM: mode %dx%d\n", fb_width, fb_height);

        /* Get encoder to find CRTC */
        struct drm_mode_get_encoder enc;
        memset(&enc, 0, sizeof(enc));
        enc.encoder_id = conn.encoder_id;
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0) {
            bm_log("[BootMenu] DRM: GETENCODER failed\n");
            continue;
        }
        drm_crtc_id = enc.crtc_id;
        /* If encoder not bound to a CRTC, find one from possible_crtcs */
        if (drm_crtc_id == 0) {
            for (int ci2 = 0; ci2 < res.count_crtcs; ci2++) {
                if (enc.possible_crtcs & (1 << ci2)) {
                    drm_crtc_id = crtc_ids[ci2];
                    break;
                }
            }
            /* Fallback: use first CRTC */
            if (drm_crtc_id == 0 && res.count_crtcs > 0) {
                drm_crtc_id = crtc_ids[0];
            }
        }
        bm_log("[BootMenu] DRM: crtc_id=%d (possible_crtcs=0x%x)\n", drm_crtc_id, enc.possible_crtcs);

        /* Create two dumb buffers for page-flip double buffering */
        uint32_t chosen_format = DRM_FORMAT_XBGR8888;
        for (int bi = 0; bi < DRM_BUFFERS; bi++) {
            struct drm_mode_create_dumb create;
            memset(&create, 0, sizeof(create));
            create.width = fb_width;
            create.height = fb_height;
            create.bpp = 32;
            if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
                bm_log("[BootMenu] DRM: CREATE_DUMB %d failed: %s\n", bi, strerror(errno));
                goto next_connector;
            }
            drm_bo_handles[bi] = create.handle;
            if (bi == 0) {
                fb_line_length = create.pitch;
                fb_size = create.size;
            }
            /* Map */
            struct drm_mode_map_dumb map;
            memset(&map, 0, sizeof(map));
            map.handle = create.handle;
            if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
                bm_log("[BootMenu] DRM: MAP_DUMB %d failed: %s\n", bi, strerror(errno));
                goto next_connector;
            }
            drm_bufs[bi] = mmap(NULL, create.size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, drm_fd, map.offset);
            if (drm_bufs[bi] == MAP_FAILED) {
                bm_log("[BootMenu] DRM: mmap %d failed: %s\n", bi, strerror(errno));
                drm_bufs[bi] = NULL;
                goto next_connector;
            }
            memset(drm_bufs[bi], 0, create.size);
            /* Add framebuffer */
            struct drm_mode_fb_cmd2 fbc;
            memset(&fbc, 0, sizeof(fbc));
            fbc.width = fb_width;
            fbc.height = fb_height;
            fbc.pixel_format = chosen_format;
            fbc.handles[0] = create.handle;
            fbc.pitches[0] = create.pitch;
            fbc.offsets[0] = 0;
            if (ioctl(drm_fd, DRM_IOCTL_MODE_ADDFB2, &fbc) < 0) {
                /* Try XRGB */
                chosen_format = DRM_FORMAT_XRGB8888;
                fbc.pixel_format = chosen_format;
                if (ioctl(drm_fd, DRM_IOCTL_MODE_ADDFB2, &fbc) < 0) {
                    bm_log("[BootMenu] DRM: ADDFB2 %d failed: %s\n", bi, strerror(errno));
                    goto next_connector;
                }
            }
            drm_fb_ids[bi] = fbc.fb_id;
        }
        bm_log("[BootMenu] DRM: %d dumb buffers, pitch=%d size=%d, fmt=%s\n",
               DRM_BUFFERS, fb_line_length, fb_size,
               chosen_format == DRM_FORMAT_XBGR8888 ? "XBGR8888" : "XRGB8888");
        /* Drawing always goes to buffer 1 (back) initially; buffer 0 is
           displayed by the initial SETCRTC (zeroed = black) */
        fb_mem = drm_bufs[1];

        /* Set CRTC with first buffer. Do disable->enable cycle to cleanly
           take over from bootloader continuous splash */
        uint32_t conn_id_for_set = conn_ids[ci];
        {
            /* Disable first */
            struct drm_mode_crtc off;
            memset(&off, 0, sizeof(off));
            off.crtc_id = drm_crtc_id;
            off.fb_id = 0;
            off.set_connectors_ptr = (uint64_t)(unsigned long)&conn_id_for_set;
            off.count_connectors = 1;
            ioctl(drm_fd, DRM_IOCTL_MODE_SETCRTC, &off);
            usleep(100000);
        }
        struct drm_mode_crtc set;
        memset(&set, 0, sizeof(set));
        set.crtc_id = drm_crtc_id;
        set.fb_id = drm_fb_ids[0];
        set.mode = drm_mode;
        set.mode_valid = 1;
        set.set_connectors_ptr = (uint64_t)(unsigned long)&conn_id_for_set;
        set.count_connectors = 1;
        if (ioctl(drm_fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
            bm_log("[BootMenu] DRM: SETCRTC failed: %s (crtc=%d conn=%d)\n",
                   strerror(errno), drm_crtc_id, conn_id_for_set);
            goto next_connector;
        }
        /* Read back CRTC state to verify */
        struct drm_mode_crtc getc;
        memset(&getc, 0, sizeof(getc));
        getc.crtc_id = drm_crtc_id;
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCRTC, &getc) == 0) {
            bm_log("[BootMenu] DRM: GETCRTC fb_id=%u mode_valid=%u x=%u y=%u\n",
                   getc.fb_id, getc.mode_valid, getc.x, getc.y);
        }
        drm_front = 0;

        /* Set connector DPMS to ON (required for some displays) */
        drm_connector_id = conn_ids[ci];
        {
            struct drm_mode_obj_get_properties props;
            memset(&props, 0, sizeof(props));
            props.obj_id = conn_ids[ci];
            props.obj_type = DRM_MODE_OBJECT_CONNECTOR;
            /* First call to get count */
            ioctl(drm_fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props);
            if (props.count_props > 0) {
                uint32_t *prop_ids = calloc(props.count_props, sizeof(uint32_t));
                uint64_t *prop_vals = calloc(props.count_props, sizeof(uint64_t));
                if (prop_ids && prop_vals) {
                    props.props_ptr = (uint64_t)(unsigned long)prop_ids;
                    props.prop_values_ptr = (uint64_t)(unsigned long)prop_vals;
                    ioctl(drm_fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props);
                    for (int pi = 0; pi < (int)props.count_props; pi++) {
                        struct drm_mode_get_property pinfo;
                        memset(&pinfo, 0, sizeof(pinfo));
                        pinfo.prop_id = prop_ids[pi];
                        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPROPERTY, &pinfo) >= 0) {
                            if (strcmp(pinfo.name, "DPMS") == 0) {
                                struct drm_mode_obj_set_property setp;
                                memset(&setp, 0, sizeof(setp));
                                setp.obj_id = conn_ids[ci];
                                setp.obj_type = DRM_MODE_OBJECT_CONNECTOR;
                                setp.prop_id = prop_ids[pi];
                                setp.value = 0; /* DRM_MODE_DPMS_ON */
                                int ret = ioctl(drm_fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &setp);
                                bm_log("[BootMenu] DRM: DPMS set ON: %s\n",
                                       ret < 0 ? strerror(errno) : "ok");
                                break;
                            }
                        }
                    }
                }
                free(prop_ids);
                free(prop_vals);
            }
        }

        bm_log("[BootMenu] DRM: display initialized %dx%d, pitch=%d\n",
               fb_width, fb_height, fb_line_length);
        found = 1;
        break;

    next_connector:
        /* Cleanup partial buffers from failed connector */
        for (int ci2 = 0; ci2 < DRM_BUFFERS; ci2++) {
            if (drm_bufs[ci2]) { munmap(drm_bufs[ci2], fb_size); drm_bufs[ci2] = NULL; }
            if (drm_fb_ids[ci2]) {
                uint32_t rm_id = drm_fb_ids[ci2];
                ioctl(drm_fd, DRM_IOCTL_MODE_RMFB, &rm_id);
                drm_fb_ids[ci2] = 0;
            }
            if (drm_bo_handles[ci2]) {
                struct drm_mode_destroy_dumb dest;
                memset(&dest, 0, sizeof(dest));
                dest.handle = drm_bo_handles[ci2];
                ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dest);
                drm_bo_handles[ci2] = 0;
            }
        }
    }

    free(conn_ids);
    free(crtc_ids);
    free(enc_ids);

    if (!found) {
        bm_log("[BootMenu] DRM: No connected display found\n");
        close(drm_fd);
        drm_fd = -1;
        return -1;
    }

    /* Turn on backlight - try multiple paths */
    const char *bl_paths[] = {
        "/sys/class/backlight/panel0-backlight/brightness",
        "/sys/class/backlight/backlight/brightness",
        "/sys/class/leds/lcd-backlight/brightness",
        "/sys/class/backlight/panel0-backlight/actual_brightness",
        NULL
    };
    for (int bi = 0; bl_paths[bi]; bi++) {
        int bl_fd = open(bl_paths[bi], O_WRONLY);
        if (bl_fd >= 0) {
            write(bl_fd, "255", 3);
            close(bl_fd);
            bm_log("[BootMenu] DRM: backlight set via %s\n", bl_paths[bi]);
            break;
        }
    }

    display_is_drm = 1;
    return 0;
}

/* Flush DRM via page flip (double buffering) - required for msm DSI panels */
static void drm_flush(void) {
    if (!display_is_drm || drm_fd < 0) return;
    int back = 1 - drm_front;
    if (drm_fb_ids[back] == 0 || !drm_bufs[back]) return;
    msync(drm_bufs[back], fb_size, MS_SYNC);
    struct drm_mode_crtc_page_flip pf;
    memset(&pf, 0, sizeof(pf));
    pf.crtc_id = drm_crtc_id;
    pf.fb_id = drm_fb_ids[back];
    pf.flags = DRM_MODE_PAGE_FLIP_EVENT;
    int ret = ioctl(drm_fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf);
    if (ret < 0 && errno == EBUSY) {
        struct pollfd pfd0 = {drm_fd, POLLIN, 0};
        poll(&pfd0, 1, 100);
        char ev0[64];
        if (pfd0.revents & POLLIN) read(drm_fd, ev0, sizeof(ev0));
        ret = ioctl(drm_fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf);
    }
    static int flip_log = 0;
    if (ret < 0) {
        if (flip_log < 5) {
            bm_log("[BootMenu] DRM: PAGE_FLIP failed: %s (errno=%d)\n",
                   strerror(errno), errno);
            flip_log++;
        }
        /* Fallback: SETCRTC with back buffer */
        struct drm_mode_crtc set;
        memset(&set, 0, sizeof(set));
        set.crtc_id = drm_crtc_id;
        set.fb_id = drm_fb_ids[back];
        set.mode = drm_mode;
        set.mode_valid = 1;
        uint32_t cid = drm_connector_id;
        set.set_connectors_ptr = (uint64_t)(unsigned long)&cid;
        set.count_connectors = 1;
        int sr = ioctl(drm_fd, DRM_IOCTL_MODE_SETCRTC, &set);
        if (flip_log < 5) {
            bm_log("[BootMenu] DRM: fallback SETCRTC ret=%d %s\n",
                   sr, sr < 0 ? strerror(errno) : "ok");
            flip_log++;
        }
    } else {
        if (flip_log < 3) {
            bm_log("[BootMenu] DRM: PAGE_FLIP ok to fb %u\n", drm_fb_ids[back]);
            flip_log++;
        }
        /* Wait for flip-complete event */
        struct pollfd pfd = {drm_fd, POLLIN, 0};
        if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN)) {
            char evbuf[64];
            read(drm_fd, evbuf, sizeof(evbuf));
        }
    }
    drm_front = back;
    fb_mem = drm_bufs[1 - drm_front];
    memset(fb_mem, 0, fb_size);
}

/* Color palette (RGB) */
#define COLOR_BG_TOP      0x1a1a2e
#define COLOR_BG_BOT      0x16213e
#define COLOR_PANEL       0x0f3460
#define COLOR_PANEL_BORDER 0xe94560
#define COLOR_TEXT        0xeeeeee
#define COLOR_TEXT_DIM    0x9aa4b2
#define COLOR_HIGHLIGHT   0xe94560
#define COLOR_HIGHLIGHT_TEXT 0xffffff
#define COLOR_PROGRESS_BG 0x333333
#define COLOR_PROGRESS_FG 0x4CAF50
#define COLOR_TITLE       0xffd700
#define COLOR_STATUS_OK   0x4CAF50
#define COLOR_STATUS_ERR  0xf44336
#define COLOR_ACTION      0x2b6f8e

/* Runtime theme colors (overridable via theme.conf) */
static unsigned int T_bg_top = COLOR_BG_TOP, T_bg_bot = COLOR_BG_BOT;
static unsigned int T_panel = COLOR_PANEL, T_border = COLOR_PANEL_BORDER;
static unsigned int T_text = COLOR_TEXT, T_dim = COLOR_TEXT_DIM;
static unsigned int T_hi = COLOR_HIGHLIGHT, T_hi_text = COLOR_HIGHLIGHT_TEXT;
static unsigned int T_title = COLOR_TITLE, T_accent = COLOR_HIGHLIGHT;

/* Desktop background image (RGB, scaled to screen); NULL = gradient */
static unsigned char *bg_buf = NULL;

/* Config settings */
static int    cfg_timeout = 0;          /* countdown seconds, 0 = disabled */
static char   cfg_default_name[MAX_NAME_LEN] = "";
static char   cfg_theme_path[MAX_PATH_LEN] = "";
static int    rom_count = 0;            /* number of ROM entries (before actions) */
static int    default_item_idx = -1;    /* resolved default ROM index */

/* Battery */
static int    battery_pct = -1;
static int    battery_charging = 0;

/* ============ Menu Entry ============ */
typedef struct {
    char name[MAX_NAME_LEN];
    char boot_img[MAX_PATH_LEN];
    char dtbo_img[MAX_PATH_LEN];
    int  type;   /* ITEM_ROM / ITEM_RECOVERY / ... */
    int  valid;
} boot_entry_t;

static boot_entry_t *entries = NULL;   /* dynamic: unlimited number of systems */
static int entry_cap = 0;
static int entry_count = 0;
static int selected = 0;

/* Ensure entries[] has room for at least 'need' items (grows by doubling) */
static void ensure_entry_cap(int need) {
    if (need <= entry_cap) return;
    int nc = entry_cap ? entry_cap : 16;
    while (nc < need) nc *= 2;
    boot_entry_t *np = realloc(entries, (size_t)nc * sizeof(boot_entry_t));
    if (!np) return;
    entries = np;
    entry_cap = nc;
}

/* ============ Font (8x16 ASCII built-in) ============ */
/* Standard VGA 8x16 bitmap font for ASCII 0-127 */
static const unsigned char font8x16[128][16] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x00
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x01
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x02
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x03
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x04
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x05
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x06
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x07
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x08 backspace
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x09 tab
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x0A LF
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x0B VT
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x0C FF
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x0D CR
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x0E
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x0F
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x10
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x11
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x12
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x13
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x14
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x15
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x16
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x17
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x18
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x19
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x1A
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x1B
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x1C
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x1D
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x1E
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0x1F
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00}, // !
    {0x00,0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // "
    {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00}, // #
    {0x00,0x00,0x18,0x7E,0xC0,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00}, // $
    {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x02,0x00,0x00,0x00}, // %
    {0x00,0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00}, // &
    {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '
    {0x00,0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00}, // (
    {0x00,0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00}, // )
    {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00}, // *
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00}, // ,
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, // .
    {0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00,0x00}, // /
    {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xCE,0xD6,0xE6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, // 0
    {0x00,0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00}, // 1
    {0x00,0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xFE,0x00,0x00,0x00,0x00}, // 2
    {0x00,0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00}, // 3
    {0x00,0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00}, // 4
    {0x00,0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00}, // 5
    {0x00,0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, // 6
    {0x00,0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x00,0x00,0x00,0x00}, // 7
    {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, // 8
    {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00}, // 9
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, // :
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00}, // ;
    {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00}, // <
    {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // =
    {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00}, // >
    {0x00,0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00}, // ?
    {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00,0x00}, // @
    {0x00,0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, // A
    {0x00,0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00}, // B
    {0x00,0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00}, // C
    {0x00,0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00}, // D
    {0x00,0x00,0x00,0xFE,0x62,0x60,0x68,0x78,0x68,0x60,0x62,0xFE,0x00,0x00,0x00,0x00}, // E
    {0x00,0x00,0x00,0xFE,0x62,0x60,0x68,0x78,0x68,0x60,0x60,0xF0,0x00,0x00,0x00,0x00}, // F
    {0x00,0x00,0x00,0x3C,0x66,0xC2,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00}, // G
    {0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, // H
    {0x00,0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, // I
    {0x00,0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00}, // J
    {0x00,0x00,0x00,0xE6,0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00}, // K
    {0x00,0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x62,0x62,0xFE,0x00,0x00,0x00,0x00}, // L
    {0x00,0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, // M
    {0x00,0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, // N
    {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, // O
    {0x00,0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00}, // P
    {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00,0x00}, // Q
    {0x00,0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00}, // R
    {0x00,0x00,0x00,0x7C,0xC6,0x60,0x30,0x7C,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00}, // S
    {0x00,0x00,0x00,0x7E,0x7E,0x5A,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, // T
    {0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, // U
    {0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00}, // V
    {0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xFE,0xEE,0x6C,0x00,0x00,0x00,0x00}, // W
    {0x00,0x00,0x00,0xC6,0xC6,0x6C,0x7C,0x38,0x7C,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00}, // X
    {0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, // Y
    {0x00,0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xFE,0x00,0x00,0x00,0x00}, // Z
    {0x00,0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00}, // [
    {0x00,0x00,0x00,0x00,0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00,0x00,0x00,0x00}, // backslash
    {0x00,0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00}, // ]
    {0x00,0x00,0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00}, // _
    {0x00,0x00,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // `
    {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00}, // a
    {0x00,0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00}, // b
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00}, // c
    {0x00,0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00}, // d
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xFE,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00}, // e
    {0x00,0x00,0x00,0x1C,0x36,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00}, // f
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00,0x00,0x00}, // g
    {0x00,0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00}, // h
    {0x00,0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, // i
    {0x00,0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00,0x00}, // j
    {0x00,0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0xE6,0x00,0x00,0x00,0x00}, // k
    {0x00,0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, // l
    {0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00}, // m
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00}, // n
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, // o
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00,0x00,0x00}, // p
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00,0x00,0x00}, // q
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00}, // r
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00}, // s
    {0x00,0x00,0x00,0x10,0x30,0x30,0x7C,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00}, // t
    {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00}, // u
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00}, // v
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0x00,0x00,0x00,0x00}, // w
    {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // x
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, // y
    {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00}, // z
    {0x00,0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00}, // {
    {0x00,0x00,0x00,0x0C,0x0C,0x0C,0x00,0x00,0x00,0x0C,0x0C,0x0C,0x00,0x00,0x00,0x00}, // |
    {0x00,0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00}, // }
    {0x00,0x00,0x00,0x00,0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ~
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // DEL
};

/* Extended font for Unicode (loaded from font.hex, Unifont format) */
typedef struct {
    unsigned int codepoint;
    unsigned char width;   /* 8 or 16 */
    unsigned char data[32]; /* 16 rows x up to 16 bits = 32 bytes max */
} glyph_t;

static glyph_t *ext_glyphs = NULL;
static int ext_glyph_count = 0;
static int ext_glyph_capacity = 0;

/* Built-in 16x16 glyphs (CJK labels render without external font):
   半杯苏打 引导电源模式重启关机量默认秒后启动充中 */
static const glyph_t builtin_glyphs[] = {
    {0x534A, 16, {0x01,0x80,0x11,0x88,0x09,0x98,0x0D,0x90,0x01,0x80,0x3F,0xFC,0x01,0x80,0x01,0x80,0x01,0x80,0x7F,0xFE,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0x00,0x00}}, /* 半 */
    {0x676F, 16, {0x10,0x00,0x13,0xFE,0x10,0x20,0x7C,0x20,0x10,0x60,0x18,0x68,0x3C,0xE8,0x34,0xA4,0x35,0xA4,0x53,0x22,0x50,0x20,0x10,0x20,0x10,0x20,0x10,0x20,0x00,0x00,0x00,0x00}}, /* 杯 */
    {0x82CF, 16, {0x04,0x20,0x04,0x20,0x7F,0xFE,0x04,0x20,0x05,0x20,0x01,0x00,0x3F,0xF8,0x01,0x18,0x03,0x1C,0x22,0x1E,0x66,0x12,0x4C,0x12,0x18,0x10,0x70,0xF0,0x00,0x00,0x00,0x00}}, /* 苏 */
    {0x6253, 16, {0x10,0x00,0x11,0xFE,0x10,0x18,0x7C,0x18,0x10,0x18,0x10,0x18,0x10,0x18,0x1C,0x18,0x70,0x18,0x50,0x18,0x10,0x18,0x10,0x18,0x10,0x18,0x70,0x70,0x00,0x00}}, /* 打 */
    {0x5F15, 16, {0x00,0x0C,0x3F,0x0C,0x01,0x0C,0x01,0x0C,0x3F,0x0C,0x20,0x0C,0x20,0x0C,0x20,0x0C,0x3F,0x8C,0x01,0x8C,0x01,0x8C,0x01,0x0C,0x01,0x0C,0x1F,0x0C,0x00,0x00,0x00,0x00}}, /* 引 */
    {0x5BFC, 16, {0x3F,0xF8,0x30,0x08,0x3F,0xF8,0x3F,0xF8,0x30,0x04,0x3F,0xFC,0x0F,0xF8,0x00,0x10,0x7F,0xFE,0x00,0x10,0x0C,0x10,0x06,0x10,0x00,0x70,0x00,0x00,0x00,0x00}}, /* 导 */
    {0x7535, 16, {0x01,0x00,0x01,0x00,0x3F,0xF8,0x21,0x08,0x21,0x08,0x21,0x08,0x3F,0xF8,0x21,0x08,0x21,0x08,0x3F,0xF8,0x21,0x00,0x01,0x02,0x01,0x02,0x01,0xFE,0x00,0x00}}, /* 电 */
    {0x6E90, 16, {0x07,0xFE,0x27,0xFE,0x14,0x20,0x05,0xFC,0x45,0x04,0x65,0xFC,0x05,0xFC,0x05,0x04,0x05,0xFC,0x34,0x30,0x24,0xBC,0x2D,0xB6,0x6B,0x32,0x58,0xE0,0x00,0x00}}, /* 源 */
    {0x6A21, 16, {0x10,0xC8,0x13,0xFE,0x10,0xC8,0x7C,0xC8,0x11,0xFC,0x19,0x04,0x3D,0xFC,0x35,0x04,0x35,0xFC,0x50,0x20,0x53,0xFE,0x10,0x50,0x10,0xCC,0x13,0x86,0x00,0x00}}, /* 模 */
    {0x5F0F, 16, {0x00,0x48,0x00,0x4C,0x00,0x40,0x7F,0xFE,0x00,0x40,0x00,0x40,0x3F,0x60,0x3F,0x60,0x08,0x20,0x08,0x20,0x08,0x32,0x09,0x92,0x7F,0x1A,0x40,0x0C,0x00,0x00}}, /* 式 */
    {0x91CD, 16, {0x00,0x00,0x00,0x3C,0x3F,0xF0,0x01,0x80,0x7F,0xFE,0x01,0x80,0x3F,0xFC,0x21,0x84,0x3F,0xFC,0x21,0x84,0x3F,0xFC,0x01,0x80,0x7F,0xFC,0x01,0x80,0x7F,0xFE,0x00,0x00}}, /* 重 */
    {0x542F, 16, {0x01,0x80,0x00,0x80,0x1F,0xFC,0x10,0x04,0x10,0x04,0x1F,0xFC,0x1F,0xFC,0x10,0x00,0x17,0xFC,0x17,0xFC,0x34,0x04,0x34,0x04,0x67,0xFC,0x44,0x04,0x04,0x00,0x00,0x00}}, /* 启 */
    {0x5173, 16, {0x08,0x10,0x0C,0x30,0x04,0x20,0x3F,0xFC,0x01,0x00,0x01,0x00,0x01,0x00,0x7F,0xFE,0x01,0x80,0x01,0xC0,0x03,0x40,0x06,0x30,0x1C,0x1C,0x70,0x0E,0x00,0x00}}, /* 关 */
    {0x673A, 16, {0x18,0x00,0x18,0xF8,0x18,0x88,0x7E,0x88,0x18,0x88,0x18,0x88,0x3C,0x88,0x3F,0x88,0x39,0x88,0x59,0x88,0x59,0x08,0x1B,0x0A,0x1A,0x0A,0x1E,0x0E,0x00,0x00}}, /* 机 */
    {0x91CF, 16, {0x1F,0xF8,0x10,0x08,0x1F,0xF8,0x10,0x08,0x1F,0xF8,0x7F,0xFE,0x00,0x00,0x1F,0xF8,0x1F,0xF8,0x11,0x88,0x1F,0xF8,0x3F,0xFC,0x01,0x80,0x7F,0xFE,0x00,0x00}}, /* 量 */
    {0x9ED8, 16, {0x7F,0x10,0x7F,0x14,0x4B,0x12,0x5D,0x10,0x49,0x7E,0x7F,0x7E,0x08,0x18,0x7F,0x18,0x08,0x18,0x1F,0x38,0x70,0x2C,0x15,0x24,0x7E,0xE6,0x4A,0xC2,0x00,0x00}}, /* 默 */
    {0x8BA4, 16, {0x30,0x20,0x10,0x20,0x18,0x20,0x00,0x20,0x00,0x60,0x70,0x60,0x10,0x60,0x10,0x60,0x10,0x50,0x10,0x50,0x14,0xD8,0x15,0x88,0x1B,0x04,0x06,0x06,0x00,0x00,0x00,0x00}}, /* 认 */
    {0x79D2, 16, {0x04,0x20,0x78,0x20,0x50,0xAC,0x11,0xA4,0x7F,0xA6,0x11,0x22,0x1B,0x22,0x35,0x24,0x34,0x2C,0x70,0x28,0xD0,0x18,0x10,0x30,0x10,0xC0,0x17,0x80,0x00,0x00}}, /* 秒 */
    {0x540E, 16, {0x00,0x18,0x3F,0xF8,0x30,0x00,0x30,0x00,0x3F,0xFE,0x30,0x00,0x30,0x00,0x30,0x00,0x37,0xFC,0x34,0x0C,0x24,0x0C,0x24,0x0C,0x67,0xFC,0x44,0x0C,0x00,0x00,0x00,0x00}}, /* 后 */
    {0x52A8, 16, {0x00,0x20,0x7E,0x20,0x00,0x20,0x00,0xFE,0x00,0x22,0x7E,0x22,0x10,0x22,0x10,0x22,0x36,0x62,0x22,0x46,0x62,0x46,0x7D,0xC6,0x00,0x84,0x01,0x1C,0x00,0x00}}, /* 动 */
    {0x5145, 16, {0x01,0x00,0x01,0x80,0x7F,0xFE,0x02,0x00,0x06,0x30,0x0C,0x18,0x19,0xF8,0x3E,0x44,0x04,0x40,0x04,0x40,0x0C,0x40,0x08,0x42,0x30,0x42,0x60,0x7E,0x00,0x00,0x00,0x00}}, /* 充 */
    {0x4E2D, 16, {0x01,0x80,0x01,0x80,0x3F,0xFC,0x3F,0xFC,0x21,0x84,0x21,0x84,0x21,0x84,0x21,0x84,0x3F,0xFC,0x21,0x84,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0x00,0x00}}, /* 中 */
};
#define BUILTIN_GLYPH_COUNT (int)(sizeof(builtin_glyphs)/sizeof(builtin_glyphs[0]))

/* ============ UTF-8 decoding ============ */
static int utf8_decode(const unsigned char **p, const unsigned char *end, unsigned int *codepoint) {
    const unsigned char *s = *p;
    if (s >= end) return -1;
    unsigned char c = *s;
    if (c < 0x80) {
        *codepoint = c;
        *p = s + 1;
        return 0;
    }
    if ((c & 0xE0) == 0xC0) {
        if (s + 1 >= end) return -1;
        *codepoint = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        *p = s + 2;
        return 0;
    }
    if ((c & 0xF0) == 0xE0) {
        if (s + 2 >= end) return -1;
        *codepoint = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p = s + 3;
        return 0;
    }
    if ((c & 0xF8) == 0xF0) {
        if (s + 3 >= end) return -1;
        *codepoint = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *p = s + 4;
        return 0;
    }
    *codepoint = 0xFFFD;
    *p = s + 1;
    return 0;
}

/* Measure UTF-8 string width in pixels */
static int text_width(const char *str) {
    int w = 0;
    const unsigned char *p = (const unsigned char *)str;
    const unsigned char *end = p + strlen(str);
    while (p < end) {
        unsigned int cp;
        if (utf8_decode(&p, end, &cp) < 0) break;
        if (cp < 128) {
            w += 8;
        } else {
            /* Search extended font */
            int found = 0;
            for (int i = 0; i < ext_glyph_count; i++) {
                if (ext_glyphs[i].codepoint == cp) {
                    w += ext_glyphs[i].width;
                    found = 1;
                    break;
                }
            }
            if (!found) w += 16; /* fallback width for unknown CJK */
        }
    }
    return w;
}

/* ============ Load Unifont hex font ============ */
static void load_ext_font(const char *path) {
    ext_glyph_capacity = 8192;
    ext_glyphs = malloc(sizeof(glyph_t) * ext_glyph_capacity);
    if (!ext_glyphs) return;
    ext_glyph_count = 0;
    /* Always seed built-in glyphs first */
    for (int b = 0; b < BUILTIN_GLYPH_COUNT; b++) {
        ext_glyphs[ext_glyph_count++] = builtin_glyphs[b];
    }
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        unsigned int cp = (unsigned int)strtoul(line, NULL, 16);
        char *hex = colon + 1;
        /* Remove trailing whitespace/newline */
        int hexlen = strlen(hex);
        while (hexlen > 0 && (hex[hexlen-1] == '\n' || hex[hexlen-1] == '\r' || hex[hexlen-1] == ' '))
            hex[--hexlen] = '\0';
        if (hexlen == 32) {
            /* 8x16 glyph: 32 hex chars = 16 bytes */
            if (ext_glyph_count >= ext_glyph_capacity) break;
            ext_glyphs[ext_glyph_count].codepoint = cp;
            ext_glyphs[ext_glyph_count].width = 8;
            for (int i = 0; i < 16; i++) {
                char byte_str[3] = {hex[i*2], hex[i*2+1], '\0'};
                ext_glyphs[ext_glyph_count].data[i] = (unsigned char)strtoul(byte_str, NULL, 16);
            }
            ext_glyph_count++;
        } else if (hexlen == 64) {
            /* 16x16 glyph: 64 hex chars = 32 bytes */
            if (ext_glyph_count >= ext_glyph_capacity) break;
            ext_glyphs[ext_glyph_count].codepoint = cp;
            ext_glyphs[ext_glyph_count].width = 16;
            for (int i = 0; i < 32; i++) {
                char byte_str[3] = {hex[i*2], hex[i*2+1], '\0'};
                ext_glyphs[ext_glyph_count].data[i] = (unsigned char)strtoul(byte_str, NULL, 16);
            }
            ext_glyph_count++;
        }
    }
    fclose(f);
}

/* ============ Framebuffer functions ============ */
static int fb_init(void) {
    display_is_drm = 0;
    /* Retry opening framebuffer - display driver may need time to initialize */
    for (int retry = 0; retry < 10; retry++) {
        fb_fd = open("/dev/graphics/fb0", O_RDWR);
        if (fb_fd < 0) fb_fd = open("/dev/fb0", O_RDWR);
        if (fb_fd < 0) {
            char path[64];
            for (int i = 0; i < 5; i++) {
                snprintf(path, sizeof(path), "/dev/fb%d", i);
                fb_fd = open(path, O_RDWR);
                if (fb_fd >= 0) break;
                snprintf(path, sizeof(path), "/dev/graphics/fb%d", i);
                fb_fd = open(path, O_RDWR);
                if (fb_fd >= 0) break;
            }
        }
        if (fb_fd >= 0) break;
        usleep(100000); /* 100ms */
    }
    if (fb_fd >= 0) {
        if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_vinfo) < 0) {
            bm_log("[BootMenu] fbdev: FBIOGET_VSCREENINFO failed, trying DRM\n");
            close(fb_fd);
            fb_fd = -1;
        } else {
            if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &fb_finfo) < 0) {
                bm_log("[BootMenu] fbdev: FBIOGET_FSCREENINFO failed, trying DRM\n");
                close(fb_fd);
                fb_fd = -1;
            } else {
                fb_width = fb_vinfo.xres;
                fb_height = fb_vinfo.yres;
                fb_bpp = fb_vinfo.bits_per_pixel;
                fb_line_length = fb_finfo.line_length;
                fb_size = fb_finfo.smem_len;
                if (fb_size <= 0) fb_size = fb_line_length * fb_height;
                bm_log("[BootMenu] fbdev: %dx%d %dbpp, line=%d, size=%d\n",
                       fb_width, fb_height, fb_bpp, fb_line_length, fb_size);
                fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
                if (fb_mem == MAP_FAILED) {
                    bm_log("[BootMenu] fbdev: mmap failed: %s, trying DRM\n", strerror(errno));
                    close(fb_fd);
                    fb_fd = -1;
                } else {
                    /* Turn on backlight */
                    int bl_fd = open("/sys/class/backlight/panel0-backlight/brightness", O_WRONLY);
                    if (bl_fd >= 0) { write(bl_fd, "255", 3); close(bl_fd); }
                    return 0;
                }
            }
        }
    }
    /* Fallback to DRM */
    bm_log("[BootMenu] fbdev not available, trying DRM...\n");
    return drm_init();
}

static void fb_close(void) {
    if (fb_mem && fb_mem != MAP_FAILED) munmap(fb_mem, fb_size);
    if (fb_fd >= 0) close(fb_fd);
    if (drm_fd >= 0) close(drm_fd);
}

static inline void put_pixel(int x, int y, unsigned int color) {
    if (x < 0 || x >= fb_width || y < 0 || y >= fb_height) return;
    unsigned char *p = fb_mem + y * fb_line_length + x * (fb_bpp / 8);
    unsigned char r = (color >> 16) & 0xFF;
    unsigned char g = (color >> 8) & 0xFF;
    unsigned char b = color & 0xFF;
    if (fb_bpp == 32) {
        if (display_is_drm) {
            /* DRM_FORMAT_XBGR8888: little-endian memory = R,G,B,X */
            p[0] = r; p[1] = g; p[2] = b; p[3] = 0xFF;
        } else {
            /* fbdev typical BGRA */
            p[0] = b; p[1] = g; p[2] = r; p[3] = 0xFF;
        }
    } else if (fb_bpp == 24) {
        p[0] = b; p[1] = g; p[2] = r;
    } else if (fb_bpp == 16) {
        /* RGB565 */
        unsigned short c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        p[0] = c & 0xFF;
        p[1] = (c >> 8) & 0xFF;
    }
}

static void fill_rect(int x, int y, int w, int h, unsigned int color) {
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            put_pixel(i, j, color);
        }
    }
}

static void draw_hline(int x, int y, int w, unsigned int color) {
    for (int i = x; i < x + w; i++) put_pixel(i, y, color);
}

static void draw_vline(int x, int y, int h, unsigned int color) {
    for (int j = y; j < y + h; j++) put_pixel(x, j, color);
}

static void draw_rect(int x, int y, int w, int h, unsigned int color) {
    draw_hline(x, y, w, color);
    draw_hline(x, y + h - 1, w, color);
    draw_vline(x, y, h, color);
    draw_vline(x + w - 1, y, h, color);
}

static void draw_gradient_bg(void) {
    if (bg_buf) {
        /* Blit pre-scaled RGB background; draw_background() adds the scrim */
        for (int y = 0; y < fb_height; y++) {
            const unsigned char *src = bg_buf + (size_t)y * fb_width * 3;
            for (int x = 0; x < fb_width; x++) {
                unsigned int color = ((unsigned int)src[x*3] << 16) |
                                     ((unsigned int)src[x*3+1] << 8) |
                                     (unsigned int)src[x*3+2];
                put_pixel(x, y, color);
            }
        }
        return;
    }
    for (int y = 0; y < fb_height; y++) {
        int t = y * 255 / fb_height;
        unsigned char r = ((T_bg_top >> 16) & 0xFF) * (255 - t) / 255 + ((T_bg_bot >> 16) & 0xFF) * t / 255;
        unsigned char g = ((T_bg_top >> 8) & 0xFF) * (255 - t) / 255 + ((T_bg_bot >> 8) & 0xFF) * t / 255;
        unsigned char b = (T_bg_top & 0xFF) * (255 - t) / 255 + (T_bg_bot & 0xFF) * t / 255;
        unsigned int color = (r << 16) | (g << 8) | b;
        for (int x = 0; x < fb_width; x++) {
            put_pixel(x, y, color);
        }
    }
}

/* Blend a solid color over existing pixels (alpha 0..255). Used over a photo bg. */
static void fill_rect_alpha(int x, int y, int w, int h, unsigned int color, int alpha) {
    if (alpha >= 255) { fill_rect(x, y, w, h, color); return; }
    if (alpha <= 0) return;
    unsigned char cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= fb_width || j < 0 || j >= fb_height) continue;
            unsigned char *p = fb_mem + j * fb_line_length + i * (fb_bpp / 8);
            unsigned char r, g, b;
            if (fb_bpp == 32 && display_is_drm) { r = p[0]; g = p[1]; b = p[2]; }
            else { b = p[0]; g = p[1]; r = p[2]; }
            r = (cr * alpha + r * (255 - alpha)) / 255;
            g = (cg * alpha + g * (255 - alpha)) / 255;
            b = (cb * alpha + b * (255 - alpha)) / 255;
            if (fb_bpp == 32 && display_is_drm) { p[0]=r; p[1]=g; p[2]=b; p[3]=0xFF; }
            else { p[0]=b; p[1]=g; p[2]=r; if (fb_bpp==32) p[3]=0xFF; }
        }
    }
}

/* Draw the desktop background: image + dark scrim, or themed gradient */
static void draw_background(void) {
    draw_gradient_bg();
    if (bg_buf) {
        /* draw_gradient_bg blitted the image; add a translucent dark scrim */
        fill_rect_alpha(0, 0, fb_width, fb_height, 0x05060d, 140);
    }
}

/* Draw a single glyph at position */
static void draw_glyph(int x, int y, unsigned int cp, unsigned int color) {
    if (cp < 128) {
        const unsigned char *glyph = font8x16[cp];
        for (int row = 0; row < 16; row++) {
            unsigned char bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    put_pixel(x + col, y + row, color);
                }
            }
        }
    } else {
        /* Search extended font */
        for (int i = 0; i < ext_glyph_count; i++) {
            if (ext_glyphs[i].codepoint == cp) {
                int gw = ext_glyphs[i].width;
                for (int row = 0; row < 16; row++) {
                    if (gw == 8) {
                        unsigned char bits = ext_glyphs[i].data[row];
                        for (int col = 0; col < 8; col++) {
                            if (bits & (0x80 >> col)) put_pixel(x + col, y + row, color);
                        }
                    } else {
                        unsigned short bits = (ext_glyphs[i].data[row*2] << 8) | ext_glyphs[i].data[row*2+1];
                        for (int col = 0; col < 16; col++) {
                            if (bits & (0x8000 >> col)) put_pixel(x + col, y + row, color);
                        }
                    }
                }
                return;
            }
        }
        /* Fallback: draw a box for unknown characters */
        draw_rect(x, y, 16, 16, color);
    }
}

/* Draw UTF-8 text */
static void draw_text(int x, int y, const char *str, unsigned int color) {
    const unsigned char *p = (const unsigned char *)str;
    const unsigned char *end = p + strlen(str);
    int cx = x;
    while (p < end) {
        unsigned int cp;
        if (utf8_decode(&p, end, &cp) < 0) break;
        int gw = (cp < 128) ? 8 : 16;
        /* Check extended font for actual width */
        if (cp >= 128) {
            for (int i = 0; i < ext_glyph_count; i++) {
                if (ext_glyphs[i].codepoint == cp) { gw = ext_glyphs[i].width; break; }
            }
        }
        draw_glyph(cx, y, cp, color);
        cx += gw;
    }
}

/* Draw centered text */
static void draw_text_centered(int cx, int y, const char *str, unsigned int color) {
    int w = text_width(str);
    draw_text(cx - w / 2, y, str, color);
}

/* ============ Theme & background (GRUB-style) ============ */
static char *bm_trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '"') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n' || e[-1] == '"')) *--e = '\0';
    return s;
}

static void bm_lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }

static void normalize_data_path(char *p) {
    if (p[0] == '/' && strncmp(p, "/data/", 6) != 0) {
        char tmp[MAX_PATH_LEN];
        snprintf(tmp, sizeof(tmp), "/data%s", p);
        strncpy(p, tmp, MAX_PATH_LEN - 1);
        p[MAX_PATH_LEN - 1] = '\0';
    }
}

static unsigned int parse_hex_color(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#') s++;
    return (unsigned int)(strtoul(s, NULL, 16) & 0xFFFFFF);
}

/* Load PNG/JPG/BMP and cover-scale (center crop) to screen size, RGB */
static unsigned char *load_image_scaled(const char *path, int dw, int dh) {
    int iw = 0, ih = 0, comp = 0;
    unsigned char *img = stbi_load(path, &iw, &ih, &comp, 3);
    if (!img) {
        bm_log("[BootMenu] Theme image load failed: %s\n", path);
        return NULL;
    }
    unsigned char *out = malloc((size_t)dw * dh * 3);
    if (!out) { stbi_image_free(img); return NULL; }
    double sa = (double)dw / iw, sb = (double)dh / ih;
    double scale = sa > sb ? sa : sb;   /* cover */
    int sw = (int)(dw / scale), sh = (int)(dh / scale);
    if (sw > iw) sw = iw;
    if (sh > ih) sh = ih;
    int sx0 = (iw - sw) / 2, sy0 = (ih - sh) / 2;
    for (int y = 0; y < dh; y++) {
        int sy = sy0 + (int)((long long)y * sh / dh);
        if (sy >= ih) sy = ih - 1;
        for (int x = 0; x < dw; x++) {
            int sx = sx0 + (int)((long long)x * sw / dw);
            if (sx >= iw) sx = iw - 1;
            const unsigned char *sp = &img[(sy * iw + sx) * 3];
            unsigned char *dp = &out[(y * dw + x) * 3];
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
        }
    }
    stbi_image_free(img);
    bm_log("[BootMenu] Theme background loaded: %s (%dx%d)\n", path, iw, ih);
    return out;
}

/* Parse a GRUB-inspired theme file: key:value / key=value, # comments */
static void load_theme(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { bm_log("[BootMenu] No theme file: %s\n", path); return; }
    char line[640];
    while (fgets(line, sizeof(line), f)) {
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '{' || *p == '}') p++;
        if (*p == '\0' || *p == '#' || *p == ';') continue;
        char *sep = strchr(p, '=');
        if (!sep) sep = strchr(p, ':');
        if (!sep) continue;
        *sep = '\0';
        char *key = bm_trim(p);
        char *val = bm_trim(sep + 1);
        char kl[64];
        strncpy(kl, key, sizeof(kl) - 1); kl[sizeof(kl) - 1] = '\0';
        bm_lower(kl);
        val = bm_trim(val);
        if (!strcmp(kl, "desktop-image") || !strcmp(kl, "background") ||
            !strcmp(kl, "background-image") || !strcmp(kl, "bg-image") ||
            !strcmp(kl, "image")) {
            char imgp[MAX_PATH_LEN];
            strncpy(imgp, val, sizeof(imgp) - 1); imgp[sizeof(imgp) - 1] = '\0';
            normalize_data_path(imgp);
            unsigned char *nb = load_image_scaled(imgp, fb_width, fb_height);
            if (nb) { if (bg_buf) free(bg_buf); bg_buf = nb; }
        } else if (val[0] == '#') {
            /* GRUB color values may look like "#fg/#bg"; take the first token */
            char *slash = strchr(val, '/');
            if (slash) *slash = '\0';
            unsigned int c = parse_hex_color(val);
            if (!strcmp(kl, "bg-top") || !strcmp(kl, "background-top")) T_bg_top = c;
            else if (!strcmp(kl, "bg-bottom") || !strcmp(kl, "bg-bot") || !strcmp(kl, "background-bottom")) T_bg_bot = c;
            else if (!strcmp(kl, "title-color")) T_title = c;
            else if (!strcmp(kl, "text-color") || !strcmp(kl, "fg-color")) T_text = c;
            else if (!strcmp(kl, "dim-color") || !strcmp(kl, "secondary-color")) T_dim = c;
            else if (!strcmp(kl, "panel-color") || !strcmp(kl, "menu-color") ||
                     !strcmp(kl, "box-color") || !strcmp(kl, "menu-bg")) T_panel = c;
            else if (!strcmp(kl, "border-color")) T_border = c;
            else if (!strcmp(kl, "highlight-color") || !strcmp(kl, "selected-color") ||
                     !strcmp(kl, "selected-bg") || !strcmp(kl, "highlight-bg")) T_hi = c;
            else if (!strcmp(kl, "selected-text-color") || !strcmp(kl, "highlight-text") ||
                     !strcmp(kl, "selected-fg")) T_hi_text = c;
            else if (!strcmp(kl, "accent-color")) T_accent = c;
        }
    }
    fclose(f);
    bm_log("[BootMenu] Theme loaded: %s\n", path);
}

/* ============ Battery ============ */
static int read_int_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static void read_battery(void) {
    battery_pct = read_int_file("/sys/class/power_supply/battery/capacity");
    FILE *s = fopen("/sys/class/power_supply/battery/status", "r");
    battery_charging = 0;
    if (s) {
        char st[40] = "";
        if (fgets(st, sizeof(st), s)) {
            if (strstr(st, "Charg") || strstr(st, "Full")) battery_charging = 1;
        }
        fclose(s);
    }
    if (battery_pct < 0 || battery_pct > 100) {
        DIR *d = opendir("/sys/class/power_supply");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                char cp[320], sp[320];
                snprintf(cp, sizeof(cp), "/sys/class/power_supply/%s/capacity", e->d_name);
                int v = read_int_file(cp);
                if (v >= 0 && v <= 100) {
                    battery_pct = v;
                    snprintf(sp, sizeof(sp), "/sys/class/power_supply/%s/status", e->d_name);
                    FILE *sf = fopen(sp, "r");
                    if (sf) { char stt[40] = ""; if (fgets(stt, sizeof(stt), sf) &&
                              (strstr(stt, "Charg") || strstr(stt, "Full"))) battery_charging = 1; fclose(sf); }
                    break;
                }
            }
            closedir(d);
        }
    }
    bm_log("[BootMenu] Battery: %d%% charging=%d\n", battery_pct, battery_charging);
}

/* Draw battery gauge + label, right-aligned ending at x_right */
static void draw_battery(int x_right, int y) {
    if (battery_pct < 0) return;
    char txt[48];
    if (battery_charging)
        snprintf(txt, sizeof(txt), "\xE5\x85\x85\xE7\x94\xB5\xE4\xB8\xAD %d%%", battery_pct); /* 充电中 */
    else
        snprintf(txt, sizeof(txt), "\xE7\x94\xB5\xE9\x87\x8F %d%%", battery_pct);           /* 电量 */
    int tw = text_width(txt);
    int icon_w = 40, icon_h = 20, gap = 8;
    int x0 = x_right - (tw + gap + icon_w);
    unsigned int col = (battery_charging || battery_pct > 20) ? COLOR_STATUS_OK : COLOR_STATUS_ERR;
    draw_text(x0, y + 2, txt, col);
    int bx = x0 + tw + gap, by = y;
    draw_rect(bx, by, icon_w - 5, icon_h, T_text);
    fill_rect(bx + icon_w - 5, by + icon_h / 2 - 3, 4, 6, T_text);
    int fw = (icon_w - 5 - 8) * battery_pct / 100;
    if (fw > 0) fill_rect(bx + 3, by + 3, fw, icon_h - 6, col);
}

/* ============ Config parsing ============ */
static int parse_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("[BootMenu] Config not found: %s\n", path);
        return -1;
    }
    char line[512];
    entry_count = 0;
    rom_count = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Remove trailing newline */
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        char *first = line;
        while (*first == ' ' || *first == '\t') first++;
        /* Skip empty lines and comments */
        if (*first == '\0' || *first == '#' || *first == ';') continue;

        if (strchr(line, '|')) {
            /* ROM entry: name|boot_img|dtbo_img */
            char *name = line;
            char *pipe1 = strchr(line, '|');
            *pipe1 = '\0';
            char *boot_img = pipe1 + 1;
            char *pipe2 = strchr(boot_img, '|');
            char *dtbo_img = "";
            if (pipe2) { *pipe2 = '\0'; dtbo_img = pipe2 + 1; }
            name = bm_trim(name); boot_img = bm_trim(boot_img); dtbo_img = bm_trim(dtbo_img);
            ensure_entry_cap(entry_count + 1);
            boot_entry_t *e = &entries[entry_count];
            memset(e, 0, sizeof(*e));
            strncpy(e->name, name, MAX_NAME_LEN - 1);
            strncpy(e->boot_img, boot_img, MAX_PATH_LEN - 1);
            strncpy(e->dtbo_img, dtbo_img, MAX_PATH_LEN - 1);
            /* Normalize paths: /setboot/... -> /data/setboot/... */
            normalize_data_path(e->boot_img);
            normalize_data_path(e->dtbo_img);
            e->type = ITEM_ROM;
            e->valid = 1;
            printf("[BootMenu] Entry %d: '%s' boot=%s dtbo=%s\n",
                   rom_count, e->name, e->boot_img, e->dtbo_img);
            entry_count++;
            rom_count++;
        } else {
            /* Setting: key=value or key:value */
            char *eq = strchr(line, '=');
            if (!eq) eq = strchr(line, ':');
            if (!eq) continue;
            *eq = '\0';
            char *key = bm_trim(line);
            char *val = bm_trim(eq + 1);
            char kl[48];
            strncpy(kl, key, sizeof(kl) - 1); kl[sizeof(kl) - 1] = '\0';
            bm_lower(kl);
            if (!strcmp(kl, "timeout") || !strcmp(kl, "countdown") || !strcmp(kl, "timeout_sec")) {
                int t = atoi(val);
                if (t < 0) t = 0; if (t > 600) t = 600;
                cfg_timeout = t;
                printf("[BootMenu] Config timeout=%d (0=disabled)\n", cfg_timeout);
            } else if (!strcmp(kl, "default") || !strcmp(kl, "default_entry")) {
                strncpy(cfg_default_name, val, MAX_NAME_LEN - 1);
                printf("[BootMenu] Config default='%s'\n", cfg_default_name);
            } else if (!strcmp(kl, "theme") || !strcmp(kl, "theme-file") || !strcmp(kl, "themefile")) {
                strncpy(cfg_theme_path, val, MAX_PATH_LEN - 1);
                normalize_data_path(cfg_theme_path);
                printf("[BootMenu] Config theme='%s'\n", cfg_theme_path);
            }
        }
    }
    fclose(f);
    return rom_count;
}

static void add_action_item(int type, const char *name) {
    ensure_entry_cap(entry_count + 1);
    boot_entry_t *e = &entries[entry_count];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, MAX_NAME_LEN - 1);
    e->type = type;
    e->valid = 1;
    entry_count++;
}

/* Append built-in action entries after the ROM list */
static void append_action_items(void) {
    add_action_item(ITEM_RECOVERY, "Recovery");
    add_action_item(ITEM_FASTBOOT, "Fastboot / Bootloader");
    add_action_item(ITEM_EDL, "EDL \xE6\xA8\xA1\xE5\xBC\x8F (9008)");          /* EDL 模式 */
    add_action_item(ITEM_REBOOT, "Reboot \xE9\x87\x8D\xE5\x90\xAF");            /* Reboot 重启 */
    add_action_item(ITEM_POWEROFF, "Poweroff \xE5\x85\xB3\xE6\x9C\xBA");        /* Poweroff 关机 */
}

/* Resolve default ROM index from cfg_default_name (1-based number or name) */
static void resolve_default(void) {
    default_item_idx = -1;
    if (rom_count <= 0) return;
    if (cfg_default_name[0]) {
        int all_digits = 1;
        for (char *c = cfg_default_name; *c; c++)
            if (!(*c >= '0' && *c <= '9') && *c != ' ') { all_digits = 0; break; }
        if (all_digits) {
            int n = atoi(cfg_default_name) - 1;
            if (n >= 0 && n < rom_count) { default_item_idx = n; return; }
        }
        for (int i = 0; i < rom_count; i++) {
            if (!strcmp(entries[i].name, cfg_default_name)) { default_item_idx = i; return; }
        }
    }
    default_item_idx = 0;
}

/* ============ Input handling ============ */
#define MAX_INPUT_DEVICES 16
static int input_fds[MAX_INPUT_DEVICES];
static int input_count = 0;
static int touch_x = -1, touch_y = -1;
static int touch_active = 0;

static int open_input_devices(void) {
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        /* Try creating and scanning */
        return -1;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && input_count < MAX_INPUT_DEVICES) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            /* Get device name */
            char name[256] = "unknown";
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            printf("[BootMenu] Input: %s -> %s\n", path, name);
            input_fds[input_count++] = fd;
        }
    }
    closedir(dir);
    return input_count;
}

static void close_input_devices(void) {
    for (int i = 0; i < input_count; i++) close(input_fds[i]);
    input_count = 0;
}

/* Input event codes */
#define INPUT_NONE    0
#define INPUT_VOL_UP  1
#define INPUT_VOL_DN  2
#define INPUT_SELECT  3
#define INPUT_TOUCH   4
#define INPUT_BACK    5

static int wait_input(int timeout_ms, int *out_touch_x, int *out_touch_y) {
    fd_set rfds;
    struct timeval tv;
    int max_fd = 0;
    FD_ZERO(&rfds);
    for (int i = 0; i < input_count; i++) {
        FD_SET(input_fds[i], &rfds);
        if (input_fds[i] > max_fd) max_fd = input_fds[i];
    }
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int ret = select(max_fd + 1, &rfds, NULL, NULL, &tv);
    if (ret <= 0) return INPUT_NONE;
    for (int i = 0; i < input_count; i++) {
        if (!FD_ISSET(input_fds[i], &rfds)) continue;
        struct input_event ev;
        while (read(input_fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_KEY) {
                if (ev.value == 1) { /* key down */
                    switch (ev.code) {
                        case KEY_VOLUMEUP: return INPUT_VOL_UP;
                        case KEY_VOLUMEDOWN: return INPUT_VOL_DN;
                        case KEY_POWER: return INPUT_SELECT;
                        case KEY_HOME: return INPUT_SELECT;
                        case KEY_BACK: return INPUT_BACK;
                        case KEY_ENTER: return INPUT_SELECT;
                        case KEY_MENU: return INPUT_SELECT;
                    }
                }
            } else if (ev.type == EV_ABS) {
                if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
                    touch_x = ev.value;
                } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                    touch_y = ev.value;
                } else if (ev.code == ABS_MT_TRACKING_ID) {
                    if (ev.value >= 0) {
                        touch_active = 1;
                    } else {
                        if (touch_active && touch_x >= 0 && touch_y >= 0) {
                            touch_active = 0;
                            if (out_touch_x) *out_touch_x = touch_x;
                            if (out_touch_y) *out_touch_y = touch_y;
                            touch_x = -1; touch_y = -1;
                            return INPUT_TOUCH;
                        }
                        touch_active = 0;
                    }
                }
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                /* For single-touch devices, detect touch release via BTN_TOUCH */
            }
            if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                if (ev.value == 0 && touch_x >= 0 && touch_y >= 0) {
                    if (out_touch_x) *out_touch_x = touch_x;
                    if (out_touch_y) *out_touch_y = touch_y;
                    touch_x = -1; touch_y = -1;
                    return INPUT_TOUCH;
                }
            }
        }
    }
    return INPUT_NONE;
}

/* ============ Partition flashing ============ */
static int flash_file_to_device(const char *src_path, const char *dev_path, const char *label) {
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        printf("[BootMenu] ERROR: Cannot open %s: %s\n", src_path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(src_fd, &st) < 0) {
        close(src_fd);
        return -1;
    }
    long long total = st.st_size;
    int dst_fd = open(dev_path, O_WRONLY | O_SYNC);
    if (dst_fd < 0) {
        printf("[BootMenu] ERROR: Cannot open %s: %s\n", dev_path, strerror(errno));
        close(src_fd);
        return -1;
    }
    printf("[BootMenu] Flashing %s (%lld bytes) to %s\n", label, total, dev_path);
    unsigned char buf[262144]; /* 256KB buffer */
    long long written = 0;
    ssize_t n;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        ssize_t w = write(dst_fd, buf, n);
        if (w != n) {
            printf("[BootMenu] ERROR: Write failed: %s\n", strerror(errno));
            close(src_fd);
            close(dst_fd);
            return -1;
        }
        written += w;
        /* Update progress */
        int pct = (int)(written * 100 / total);
        printf("\r[BootMenu] %s: %d%% (%lld/%lld)", label, pct, written, total);
        fflush(stdout);
    }
    printf("\n");
    fsync(dst_fd);
    close(src_fd);
    close(dst_fd);
    printf("[BootMenu] %s flash complete\n", label);
    return 0;
}

/* ============ Reboot / power actions ============ */
#include <sys/syscall.h>
#include <linux/reboot.h>

static void bm_sync(void) { sync(); }

static void sysrq_trigger(char c) {
    int fd = open("/proc/sysrq-trigger", O_WRONLY);
    if (fd >= 0) { write(fd, &c, 1); close(fd); }
}

static void write_bcb(const char *cmd) {
    int fd = open(MISC_DEV, O_WRONLY | O_SYNC);
    if (fd < 0) return;
    unsigned char msg[2048];
    memset(msg, 0, sizeof(msg));
    if (cmd) strncpy((char *)msg, cmd, sizeof(msg) - 1);
    write(fd, msg, sizeof(msg));
    fsync(fd);
    close(fd);
}

static void clear_bcb(void) { write_bcb(NULL); }

static void restart2(const char *arg) {
    bm_sync();
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
            LINUX_REBOOT_CMD_RESTART2, (char *)arg);
}

static void reboot_cmd(int cmd) {
    bm_sync();
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, cmd, NULL);
}

static void hang_forever(void) {
    while (1) sleep(1);
}

/* Boot the currently-installed recovery (OrangeFox), no flashing */
static void reboot_recovery(void) {
    bm_log("[BootMenu] Rebooting to recovery...\n");
    restart2("recovery");
    usleep(300000);
    write_bcb("boot-recovery");
    reboot_cmd(LINUX_REBOOT_CMD_RESTART);
    usleep(300000);
    sysrq_trigger('b');
    hang_forever();
}

static void reboot_fastboot(void) {
    bm_log("[BootMenu] Rebooting to fastboot/bootloader...\n");
    restart2("bootloader");
    usleep(300000);
    write_bcb("bootonce-bootloader");
    restart2("bootloader");
    usleep(300000);
    reboot_cmd(LINUX_REBOOT_CMD_RESTART);
    sysrq_trigger('b');
    hang_forever();
}

/* Qualcomm Emergency Download mode (EDL / 9008) */
static void reboot_edl(void) {
    bm_log("[BootMenu] Rebooting to EDL (9008 emergency download)...\n");
    restart2("edl");
    usleep(400000);
    restart2("emergency");
    usleep(400000);
    restart2("9008");
    usleep(400000);
    /* Some Qualcomm kernels expose an EDL restart node */
    int fd = open("/sys/class/android_usb/android0/boot_mode", O_WRONLY);
    if (fd >= 0) { write(fd, "edl", 3); close(fd); }
    bm_log("[BootMenu] EDL reboot not honored by kernel; falling back to fastboot\n");
    reboot_fastboot();
    hang_forever();
}

static void reboot_system(void) {
    bm_log("[BootMenu] Normal reboot (back to BootMenu on boot partition)...\n");
    clear_bcb();
    reboot_cmd(LINUX_REBOOT_CMD_RESTART);
    usleep(300000);
    restart2("");
    sysrq_trigger('b');
    hang_forever();
}

static void power_off(void) {
    bm_log("[BootMenu] Powering off...\n");
    reboot_cmd(LINUX_REBOOT_CMD_POWER_OFF);
    usleep(300000);
    restart2("poweroff");
    usleep(300000);
    sysrq_trigger('o');
    hang_forever();
}

/* Execute the selected menu entry (ROM flash or built-in action) */
static void activate_entry(int idx) {
    boot_entry_t *ent = &entries[idx];
    bm_log("[BootMenu] Activate idx=%d type=%d name='%s'\n", idx, ent->type, ent->name);
    switch (ent->type) {
        case ITEM_RECOVERY:
            /* Not reached from main: ITEM_RECOVERY exits the menu (see activate:).
             * Kept here only as a defensive fallback. */
            fb_close(); close_input_devices();
            reboot_recovery();
            break;
        case ITEM_FASTBOOT:
            fb_close(); close_input_devices();
            reboot_fastboot();
            break;
        case ITEM_EDL:
            fb_close(); close_input_devices();
            reboot_edl();
            break;
        case ITEM_REBOOT:
            fb_close(); close_input_devices();
            reboot_system();
            break;
        case ITEM_POWEROFF:
            fb_close(); close_input_devices();
            power_off();
            break;
        case ITEM_ROM:
        default:
            break; /* handled by caller (flash flow) */
    }
}

/* Back-compat name used by the flash flow */
static void do_reboot(void) { reboot_recovery(); }

/* ============ Menu rendering ============ */
/* Menu item geometry (dynamic: unlimited entries, scrollable list) */
typedef struct { int header; int item; int y; int h; } row_t;
static row_t *menu_rows = NULL;
static int menu_rows_cap = 0;
static int menu_row_count = 0;
static int menu_max_visible = 1;   /* max item rows that fit on one screen */
static int menu_total_rows = 1;    /* total global rows incl. section header */
static int scroll_top = 0;         /* first visible global row index */
static int menu_item_height = 64;
static int menu_item_width = 0;
static int menu_x = 0;
static int menu_y = 0;
static int menu_gap = 12;
static int menu_header_h = 44;

static void ensure_rows_cap(int need) {
    if (need <= menu_rows_cap) return;
    int nc = menu_rows_cap ? menu_rows_cap : 24;
    while (nc < need) nc *= 2;
    row_t *np = realloc(menu_rows, (size_t)nc * sizeof(row_t));
    if (!np) return;
    menu_rows = np;
    menu_rows_cap = nc;
}

/* Global row index of an entry (the section header occupies the slot at rom_count) */
static int row_of(int item) {
    if (rom_count > 0 && rom_count < entry_count && item >= rom_count) return item + 1;
    return item;
}

/* Map a global row index to an item index, or -1 for the section header */
static int item_of_row(int r) {
    if (rom_count > 0 && rom_count < entry_count) {
        if (r == rom_count) return -1;
        if (r > rom_count) return r - 1;
    }
    return r;
}

/* Build the row descriptors for the visible window starting at scroll_top */
static void build_visible_rows(int start_y) {
    int scrolling = (menu_total_rows > menu_max_visible);
    int visible = scrolling ? menu_max_visible : menu_total_rows;
    ensure_rows_cap(menu_max_visible + 2);
    menu_row_count = 0;
    int y = start_y;
    for (int i = 0; i < visible; i++) {
        int gr = scroll_top + i;
        if (gr >= menu_total_rows) break;
        int item = item_of_row(gr);
        if (item < 0) {
            menu_rows[menu_row_count].header = 1;
            menu_rows[menu_row_count].item = -1;
            menu_rows[menu_row_count].y = y;
            menu_rows[menu_row_count].h = menu_header_h;
            menu_row_count++;
            y += menu_header_h;
            continue;
        }
        menu_rows[menu_row_count].header = 0;
        menu_rows[menu_row_count].item = item;
        menu_rows[menu_row_count].y = y;
        menu_rows[menu_row_count].h = menu_item_height;
        menu_row_count++;
        y += menu_item_height + menu_gap;
    }
}

/* Keep the selection visible, then keep scroll_top in range */
static void clamp_scroll(void) {
    if (entry_count <= 0) { scroll_top = 0; return; }
    int max_vis = menu_max_visible;
    int srow = row_of(selected);
    if (menu_total_rows <= max_vis) { scroll_top = 0; return; }
    if (srow < scroll_top) scroll_top = srow;
    else if (srow >= scroll_top + max_vis) scroll_top = srow - max_vis + 1;
    if (scroll_top < 0) scroll_top = 0;
    if (scroll_top + max_vis > menu_total_rows)
        scroll_top = menu_total_rows - max_vis;
    if (scroll_top < 0) scroll_top = 0;
    build_visible_rows(menu_y);
}

static void calc_menu_layout(void) {
    menu_item_width = fb_width * 3 / 4;
    if (menu_item_width > 820) menu_item_width = 820;
    menu_x = (fb_width - menu_item_width) / 2;
    int top_reserve = 190, bottom_reserve = 140;
    int avail = fb_height - top_reserve - bottom_reserve;
    menu_total_rows = entry_count + ((rom_count > 0 && rom_count < entry_count) ? 1 : 0);
    int n = entry_count > 0 ? entry_count : 1;
    /* Row height: fit all if possible, clamped to a readable range */
    int h = (avail - menu_header_h - n * menu_gap) / n;
    if (h > 112) h = 112;
    if (h < 56) h = 56;
    menu_item_height = h;
    menu_max_visible = (avail - menu_header_h) / (h + menu_gap);
    if (menu_max_visible < 1) menu_max_visible = 1;
    /* Determine layout mode */
    int scrolling = (menu_total_rows > menu_max_visible);
    int start_y;
    if (scrolling) {
        /* Scroll mode: fixed row height, list starts at the top reserve */
        start_y = top_reserve;
        menu_y = start_y;
        clamp_scroll();
    } else {
        scroll_top = 0;
        /* Fit mode: vertically center the whole block */
        int total_h = menu_header_h + n * (h + menu_gap);
        start_y = top_reserve + (avail - total_h) / 2;
        if (start_y < top_reserve) start_y = top_reserve;
        menu_y = start_y;
        build_visible_rows(start_y);
    }
}

/* Map a touch point to an item index, or -1 */
static int item_at_xy(int tx, int ty) {
    for (int r = 0; r < menu_row_count; r++) {
        if (menu_rows[r].header) continue;
        int y = menu_rows[r].y, h = menu_rows[r].h;
        if (ty >= y && ty <= y + h && tx >= menu_x && tx <= menu_x + menu_item_width)
            return menu_rows[r].item;
    }
    return -1;
}

static void menu_panel(int x, int y, int w, int h, unsigned int solid, int alpha) {
    if (bg_buf) fill_rect_alpha(x, y, w, h, solid, alpha);
    else        fill_rect(x, y, w, h, solid);
}

static void render_menu(int cd_remaining) {
    draw_background();
    /* Title */
    char title[64];
    snprintf(title, sizeof(title), "BootMenu v%s  by\xE5\x8D\x8A\xE6\x9D\xAF\xE8\x8B\x8F\xE6\x89\x93", BOOTMENU_VERSION);
    draw_text_centered(fb_width / 2, 56, title, T_title);
    draw_text_centered(fb_width / 2, 92, "Multi-Boot Selector", T_dim);
    /* Battery (top-right); read once via read_battery() in main */
    draw_battery(fb_width - 36, 58);

    /* Countdown / default line */
    int list_top = menu_y;
    if (cd_remaining >= 0 && default_item_idx >= 0 && default_item_idx < rom_count) {
        char cd[160];
        snprintf(cd, sizeof(cd), "\xE9\xBB\x98\xE8\xAE\xA4 %d s  %s",   /* 默认 N s */
                 cd_remaining, entries[default_item_idx].name);
        draw_text_centered(fb_width / 2, 128, cd, T_accent);
        int bar_w = menu_item_width, bar_h = 10;
        int bx = menu_x, by = 150;
        menu_panel(bx, by, bar_w, bar_h, COLOR_PROGRESS_BG, 180);
        double frac = (cfg_timeout > 0) ? (double)cd_remaining / cfg_timeout : 0;
        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        fill_rect(bx, by, (int)(bar_w * frac), bar_h, T_accent);
        list_top = menu_y + 10;
    } else {
        char sub[64];
        snprintf(sub, sizeof(sub), "%d system(s) + power actions", rom_count);
        draw_text_centered(fb_width / 2, 128, sub, T_dim);
    }
    (void)list_top;

    /* Rows */
    for (int r = 0; r < menu_row_count; r++) {
        if (menu_rows[r].header) {
            int hy = menu_rows[r].y + menu_rows[r].h / 2 - 8;
            const char *htxt = "\xE5\xBC\x95\xE5\xAF\xBC / \xE7\x94\xB5\xE6\xBA\x90"; /* 引导 / 电源 */
            int tw = text_width(htxt);
            int cx = fb_width / 2;
            draw_hline(menu_x, hy + 8, (menu_item_width - tw) / 2 - 24, T_dim);
            draw_hline(cx + tw / 2 + 24, hy + 8, (menu_item_width - tw) / 2 - 24, T_dim);
            draw_text_centered(cx, hy, htxt, T_accent);
            continue;
        }
        int idx = menu_rows[r].item;
        int y = menu_rows[r].y;
        int is_sel = (idx == selected);
        int is_action = entries[idx].type != ITEM_ROM;
        unsigned int bg_col = is_sel ? T_hi : (is_action ? COLOR_ACTION : T_panel);
        unsigned int txt_col = is_sel ? T_hi_text : T_text;
        unsigned int border_col = is_sel ? T_hi_text : (is_action ? T_accent : T_border);
        menu_panel(menu_x, y, menu_item_width, menu_item_height, bg_col, is_sel ? 220 : 170);
        draw_rect(menu_x, y, menu_item_width, menu_item_height, border_col);
        if (is_sel) fill_rect(menu_x, y, 6, menu_item_height, T_title);

        int ty = y + (menu_item_height - 16) / 2;
        if (!is_action) {
            char num[8];
            snprintf(num, sizeof(num), "%d.", idx + 1);
            draw_text(menu_x + 20, ty, num, is_sel ? T_hi_text : T_dim);
        } else {
            draw_text(menu_x + 20, ty, ">", is_sel ? T_title : T_accent);
        }
        draw_text(menu_x + 60, ty, entries[idx].name, txt_col);

        /* Default badge for ROM entries */
        if (!is_action && idx == default_item_idx) {
            const char *db = "\xE9\xBB\x98\xE8\xAE\xA4"; /* 默认 */
            int dw = text_width(db);
            draw_text(menu_x + menu_item_width - dw - 22, ty, db,
                      is_sel ? T_hi_text : T_title);
        }
    }

    /* Scroll indicators when the list is longer than the screen */
    if (menu_total_rows > menu_max_visible) {
        if (scroll_top > 0)
            draw_text_centered(fb_width / 2, menu_y - 22, "^ more", T_accent);
        int last_y = menu_y;
        for (int r = 0; r < menu_row_count; r++)
            if (menu_rows[r].y + menu_rows[r].h > last_y) last_y = menu_rows[r].y + menu_rows[r].h;
        if (scroll_top + menu_row_count < menu_total_rows)
            draw_text_centered(fb_width / 2, last_y + 6, "v more", T_accent);
    }

    /* Footer instructions */
    int footer_y = fb_height - 92;
    draw_text_centered(fb_width / 2, footer_y,
                       "Vol+/- Navigate      Power / Touch Select", T_dim);
    draw_text_centered(fb_width / 2, footer_y + 24,
                       "Recovery / Fastboot / EDL / Reboot / Poweroff in the list", T_dim);
    draw_text_centered(fb_width / 2, footer_y + 46,
                       "Config: /data/setboot/file.txt", T_dim);
    drm_flush();
}

static void render_flash_screen(const char *name, const char *status, int progress) {
    draw_background();
    draw_text_centered(fb_width / 2, fb_height / 2 - 80, "Flashing Boot Image", T_title);
    draw_text_centered(fb_width / 2, fb_height / 2 - 50, name, T_text);
    /* Progress bar */
    int bar_w = fb_width * 2 / 3;
    int bar_h = 32;
    int bar_x = (fb_width - bar_w) / 2;
    int bar_y = fb_height / 2;
    fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_PROGRESS_BG);
    if (progress > 0) {
        fill_rect(bar_x, bar_y, bar_w * progress / 100, bar_h, COLOR_PROGRESS_FG);
    }
    draw_rect(bar_x, bar_y, bar_w, bar_h, T_dim);
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", progress);
    draw_text_centered(fb_width / 2, bar_y + 8, pct, T_hi_text);
    if (status) {
        draw_text_centered(fb_width / 2, bar_y + bar_h + 20, status, T_text);
    }
    drm_flush();
}

static void render_error_screen(const char *msg) {
    draw_background();
    draw_text_centered(fb_width / 2, fb_height / 2 - 40, "ERROR", COLOR_STATUS_ERR);
    draw_text_centered(fb_width / 2, fb_height / 2, msg, T_text);
    draw_text_centered(fb_width / 2, fb_height / 2 + 40,
                       "Press Power to continue", T_dim);
    drm_flush();
}

/* ============ Main ============ */
#ifndef HOST_TEST
int main(int argc, char **argv) {
    set_selinux_permissive();
    bm_log("[BootMenu] Starting BootMenu v%s\n", BOOTMENU_VERSION);
    bm_log("[BootMenu] Device: Redmi K20 Pro (raphael)\n");
    /* Wait a moment for devices to settle */
    usleep(500000);
    /* Load extended font if available */
    load_ext_font(FONT_PATH);
    bm_log("[BootMenu] Extended font: %d glyphs loaded\n", ext_glyph_count);
    /* Initialize framebuffer */
    if (fb_init() < 0) {
        bm_log("[BootMenu] FATAL: No framebuffer, exiting to recovery\n");
        return 0;
    }
    /* Open input devices */
    open_input_devices();
    bm_log("[BootMenu] %d input devices opened\n", input_count);

    /* Parse ROM config (optional), then always append power/boot actions */
    parse_config(CONFIG_PATH);
    append_action_items();
    resolve_default();
    bm_log("[BootMenu] %d ROM(s), %d total items, default idx=%d timeout=%d\n",
           rom_count, entry_count, default_item_idx, cfg_timeout);

    /* Load GRUB-style theme: explicit path from config, else auto-detect */
    if (cfg_theme_path[0]) {
        load_theme(cfg_theme_path);
    } else {
        FILE *tf = fopen(THEME_AUTO_PATH, "r");
        if (tf) { fclose(tf); load_theme(THEME_AUTO_PATH); }
    }

    read_battery();
    calc_menu_layout();
    selected = (default_item_idx >= 0) ? default_item_idx : 0;
    clamp_scroll();

    /* Countdown auto-boot state */
    int cd_active = (cfg_timeout > 0 && default_item_idx >= 0);
    int cd_ms = cfg_timeout * 1000;
    int cd_left = cfg_timeout;
    int last_shown = cd_left;
    render_menu(cd_active ? cd_left : -1);

    /* Main menu loop */
    while (1) {
        int tx = -1, ty = -1;
        int inp = wait_input(cd_active ? 100 : 200, &tx, &ty);
        if (inp == INPUT_NONE) {
            if (cd_active) {
                cd_ms -= 100;
                int now = (cd_ms + 99) / 1000;
                if (now < 0) now = 0;
                if (now != last_shown) {
                    cd_left = now;
                    last_shown = now;
                    render_menu(cd_left);
                }
                if (cd_ms <= 0) {
                    bm_log("[BootMenu] Countdown elapsed, booting default '%s'\n",
                           entries[default_item_idx].name);
                    selected = default_item_idx;
                    goto do_flash;
                }
            }
            continue;
        }
        /* Any manual input cancels the countdown */
        int was_active = cd_active;
        cd_active = 0;
        switch (inp) {
            case INPUT_VOL_UP:
                selected = (selected - 1 + entry_count) % entry_count;
                clamp_scroll();
                render_menu(-1);
                break;
            case INPUT_VOL_DN:
                selected = (selected + 1) % entry_count;
                clamp_scroll();
                render_menu(-1);
                break;
            case INPUT_SELECT:
                goto activate;
            case INPUT_TOUCH: {
                int touch_idx = item_at_xy(tx, ty);
                if (touch_idx >= 0) {
                    selected = touch_idx;
                    goto activate;
                }
                if (was_active) render_menu(-1);
                break;
            }
            case INPUT_BACK:
                /* Back does not exit the menu; use the 'Recovery' entry instead */
                if (was_active) render_menu(-1);
                break;
            default:
                if (was_active) render_menu(-1);
                break;
        }
    }

activate:
    if (entries[selected].type == ITEM_RECOVERY) {
        /* Exit the menu: the wrapper then execs the real OrangeFox recovery */
        bm_log("[BootMenu] Recovery selected, exiting to OrangeFox recovery\n");
        fb_close();
        close_input_devices();
        return 0;
    }
    if (entries[selected].type != ITEM_ROM) {
        activate_entry(selected);   /* reboots / powers off, does not return */
        hang_forever();
    }

do_flash:
    ;
    boot_entry_t *ent = &entries[selected];
    bm_log("[BootMenu] Selected: '%s'\n", ent->name);
    bm_log("[BootMenu] boot.img: %s\n", ent->boot_img);
    bm_log("[BootMenu] dtbo.img: %s\n", ent->dtbo_img);
    /* Flash boot.img to recovery partition */
    render_flash_screen(ent->name, "Writing boot.img to recovery...", 0);
    if (flash_file_to_device(ent->boot_img, RECOVERY_DEV, "boot->recovery") < 0) {
        render_error_screen("Failed to flash boot.img to recovery partition");
        int waited = 0;
        while (waited < 8000) {
            int tx, ty;
            int in = wait_input(500, &tx, &ty);
            if (in == INPUT_SELECT || in == INPUT_TOUCH) break;
            waited += 500;
        }
        fb_close();
        close_input_devices();
        return 0;
    }
    /* Flash dtbo.img to dtbo partition */
    render_flash_screen(ent->name, "Writing dtbo.img to dtbo...", 50);
    if (ent->dtbo_img[0] != '\0') {
        if (flash_file_to_device(ent->dtbo_img, DTBO_DEV, "dtbo->dtbo") < 0) {
            render_error_screen("Failed to flash dtbo.img to dtbo partition");
            int waited = 0;
            while (waited < 8000) {
                int tx, ty;
                int in = wait_input(500, &tx, &ty);
                if (in == INPUT_SELECT || in == INPUT_TOUCH) break;
                waited += 500;
            }
            fb_close();
            close_input_devices();
            return 0;
        }
    }
    /* Done - reboot to recovery to boot chosen system */
    render_flash_screen(ent->name, "Done! Rebooting to recovery...", 100);
    fb_close();
    close_input_devices();
    do_reboot();
    /* Should not reach here */
    return 0;
}
#else
/* ================= HOST UI PREVIEW (not on device) ================= */
static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror("ppm"); return; }
    fprintf(f, "P6\n%d %d\n255\n", fb_width, fb_height);
    for (int y = 0; y < fb_height; y++) {
        unsigned char *row = fb_mem + y * fb_line_length;
        for (int x = 0; x < fb_width; x++)
            fwrite(&row[x*4], 1, 3, f); /* XBGR memory order = R,G,B */
    }
    fclose(f);
}

int main(int argc, char **argv) {
    /* args: config.txt [theme.conf] [selected_idx] out_menu.ppm out_countdown.ppm */
    fb_width = 1080; fb_height = 2340; fb_bpp = 32;
    fb_line_length = fb_width * 4; fb_size = fb_line_length * fb_height;
    fb_mem = calloc(1, fb_size);
    display_is_drm = 1; drm_fd = -1;
    load_ext_font("/nonexistent/font.hex");
    if (argc > 1) parse_config(argv[1]);
    append_action_items();
    resolve_default();
    if (argc > 2 && argv[2][0]) load_theme(argv[2]);
    battery_pct = 85; battery_charging = 0;
    calc_menu_layout();
    selected = 0;
    if (argc > 3 && argv[3][0]) {
        int s = atoi(argv[3]);
        if (s >= 0 && s < entry_count) selected = s;
    }
    clamp_scroll();
    render_menu(-1);
    if (argc > 4) write_ppm(argv[4]);
    /* countdown frame at 7s */
    battery_pct = 23; battery_charging = 1;
    render_menu(7);
    if (argc > 5) write_ppm(argv[5]);
    printf("HOST_PREVIEW ok: rom=%d total=%d default=%d timeout=%d bg=%d scroll_top=%d max_vis=%d\n",
           rom_count, entry_count, default_item_idx, cfg_timeout, bg_buf != NULL,
           scroll_top, menu_max_visible);
    return 0;
}
#endif
