/*
 * BootMenu - Android Multi-Boot Boot Menu V0.2
 * For Redmi K20 Pro (raphael) / Qualcomm msmnile
 *
 * Features:
 *   - Framebuffer visual menu with color UI
 *   - Volume +/- navigation, Power/touch to select
 *   - Touch screen support (tap to select)
 *   - Reads config from /data/setboot/file.txt
 *   - Flashes boot.img to boot partition, dtbo.img to dtbo partition
 *   - Normal reboot after selection to boot chosen system
 *   - Up to 10 boot entries, custom names
 *   - Multi-language font support (loads /data/setboot/font.hex if present)
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
#define MAX_ENTRIES     10
#define MAX_NAME_LEN    128
#define MAX_PATH_LEN    256
#define CONFIG_PATH     "/data/setboot/file.txt"
#define FONT_PATH       "/data/setboot/font.hex"
#define RECOVERY_DEV    "/dev/block/bootdevice/by-name/recovery"
#define DTBO_DEV        "/dev/block/bootdevice/by-name/dtbo"
#define BOOTMENU_VERSION "0.6"

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
#define COLOR_TEXT_DIM    0x888888
#define COLOR_HIGHLIGHT   0xe94560
#define COLOR_HIGHLIGHT_TEXT 0xffffff
#define COLOR_PROGRESS_BG 0x333333
#define COLOR_PROGRESS_FG 0x4CAF50
#define COLOR_TITLE       0xffd700
#define COLOR_STATUS_OK   0x4CAF50
#define COLOR_STATUS_ERR  0xf44336

/* ============ Menu Entry ============ */
typedef struct {
    char name[MAX_NAME_LEN];
    char boot_img[MAX_PATH_LEN];
    char dtbo_img[MAX_PATH_LEN];
    int valid;
} boot_entry_t;

static boot_entry_t entries[MAX_ENTRIES];
static int entry_count = 0;
static int selected = 0;

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

/* Built-in 16x16 glyphs: 半杯苏打 (so title renders without external font) */
static const glyph_t builtin_glyphs[] = {
    {0x534A, 16, {0x01,0x80, 0x11,0x88, 0x09,0x98, 0x0D,0x90, 0x01,0x80, 0x3F,0xFC, 0x01,0x80, 0x01,0x80, 0x01,0x80, 0x7F,0xFE, 0x01,0x80, 0x01,0x80, 0x01,0x80, 0x01,0x80, 0x01,0x80, 0x00,0x00}},
    {0x676F, 16, {0x10,0x00, 0x13,0xFE, 0x10,0x20, 0x7C,0x20, 0x10,0x60, 0x18,0x68, 0x3C,0xE8, 0x34,0xA4, 0x35,0xA4, 0x53,0x22, 0x50,0x20, 0x10,0x20, 0x10,0x20, 0x10,0x20, 0x00,0x00, 0x00,0x00}},
    {0x82CF, 16, {0x04,0x20, 0x04,0x20, 0x7F,0xFE, 0x04,0x20, 0x05,0x20, 0x01,0x00, 0x3F,0xF8, 0x01,0x18, 0x03,0x1C, 0x22,0x1E, 0x66,0x12, 0x4C,0x12, 0x18,0x10, 0x70,0xF0, 0x00,0x00, 0x00,0x00}},
    {0x6253, 16, {0x10,0x00, 0x11,0xFE, 0x10,0x18, 0x7C,0x18, 0x10,0x18, 0x10,0x18, 0x10,0x18, 0x1C,0x18, 0x70,0x18, 0x50,0x18, 0x10,0x18, 0x10,0x18, 0x10,0x18, 0x70,0x70, 0x00,0x00}},
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
    for (int y = 0; y < fb_height; y++) {
        int t = y * 255 / fb_height;
        unsigned char r = ((COLOR_BG_TOP >> 16) & 0xFF) * (255 - t) / 255 + ((COLOR_BG_BOT >> 16) & 0xFF) * t / 255;
        unsigned char g = ((COLOR_BG_TOP >> 8) & 0xFF) * (255 - t) / 255 + ((COLOR_BG_BOT >> 8) & 0xFF) * t / 255;
        unsigned char b = (COLOR_BG_TOP & 0xFF) * (255 - t) / 255 + (COLOR_BG_BOT & 0xFF) * t / 255;
        unsigned int color = (r << 16) | (g << 8) | b;
        for (int x = 0; x < fb_width; x++) {
            put_pixel(x, y, color);
        }
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

/* ============ Config parsing ============ */
static int parse_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("[BootMenu] Config not found: %s\n", path);
        return -1;
    }
    char line[512];
    entry_count = 0;
    while (fgets(line, sizeof(line), f) && entry_count < MAX_ENTRIES) {
        /* Remove trailing newline */
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        /* Skip empty lines and comments */
        if (len == 0 || line[0] == '#' || line[0] == ';') continue;
        /* Parse: name|boot_img|dtbo_img */
        char *name = line;
        char *pipe1 = strchr(line, '|');
        if (!pipe1) continue;
        *pipe1 = '\0';
        char *boot_img = pipe1 + 1;
        char *pipe2 = strchr(boot_img, '|');
        char *dtbo_img = "";
        if (pipe2) {
            *pipe2 = '\0';
            dtbo_img = pipe2 + 1;
        }
        /* Trim whitespace */
        while (*name == ' ' || *name == '\t') name++;
        char *end = name + strlen(name) - 1;
        while (end > name && (*end == ' ' || *end == '\t')) *end-- = '\0';
        strncpy(entries[entry_count].name, name, MAX_NAME_LEN - 1);
        strncpy(entries[entry_count].boot_img, boot_img, MAX_PATH_LEN - 1);
        strncpy(entries[entry_count].dtbo_img, dtbo_img, MAX_PATH_LEN - 1);
        /* Normalize paths: /setboot/... -> /data/setboot/... */
        for (int pi = 0; pi < 2; pi++) {
            char *p = (pi == 0) ? entries[entry_count].boot_img : entries[entry_count].dtbo_img;
            if (p[0] == '/' && strncmp(p, "/data/", 6) != 0) {
                char tmp[MAX_PATH_LEN];
                snprintf(tmp, sizeof(tmp), "/data%s", p);
                strncpy(p, tmp, MAX_PATH_LEN - 1);
            }
        }
        entries[entry_count].valid = 1;
        printf("[BootMenu] Entry %d: '%s' boot=%s dtbo=%s\n",
               entry_count, entries[entry_count].name,
               entries[entry_count].boot_img, entries[entry_count].dtbo_img);
        entry_count++;
    }
    fclose(f);
    return entry_count;
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

/* ============ Normal reboot ============ */
#include <sys/syscall.h>
#include <linux/reboot.h>

static void do_reboot(void) {
    bm_log("[BootMenu] Rebooting to recovery...\n");
    usleep(500000);
    /* Method 1: Linux reboot syscall RESTART2 with "recovery" */
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
            LINUX_REBOOT_CMD_RESTART2, "recovery");
    /* Method 2: Write boot-recovery to misc partition then normal reboot */
    int misc_fd = open("/dev/block/bootdevice/by-name/misc", O_WRONLY | O_SYNC);
    if (misc_fd >= 0) {
        unsigned char msg[2048];
        memset(msg, 0, sizeof(msg));
        memcpy(msg, "boot-recovery", 13);
        write(misc_fd, msg, sizeof(msg));
        fsync(misc_fd);
        close(misc_fd);
        reboot(RB_AUTOBOOT);
    }
    /* Method 3: standard reboot */
    reboot(RB_AUTOBOOT);
    /* Method 4: sysrq-trigger as last resort */
    int fd = open("/proc/sysrq-trigger", O_WRONLY);
    if (fd >= 0) {
        write(fd, "b", 1);
        close(fd);
    }
    while (1) { sleep(1); }
}

/* ============ Menu rendering ============ */
/* Menu item geometry */
static int menu_item_height = 64;
static int menu_item_width = 0;
static int menu_x = 0;
static int menu_y = 0;
static int menu_visible_count = 0;
static int menu_scroll = 0;

static void calc_menu_layout(void) {
    menu_item_width = fb_width * 3 / 4;
    if (menu_item_width > 800) menu_item_width = 800;
    menu_x = (fb_width - menu_item_width) / 2;
    menu_y = fb_height * 0.22;
    menu_item_height = 64;
    int available_h = fb_height - menu_y - 120;
    menu_visible_count = available_h / (menu_item_height + 12);
    if (menu_visible_count > entry_count) menu_visible_count = entry_count;
    if (menu_visible_count < 1) menu_visible_count = 1;
}

static void render_menu(void) {
    draw_gradient_bg();
    /* Title */
    char title[64];
    snprintf(title, sizeof(title), "BootMenu v%s  by\xE5\x8D\x8A\xE6\x9D\xAF\xE8\x8B\x8F\xE6\x89\x93", BOOTMENU_VERSION);
    draw_text_centered(fb_width / 2, menu_y - 80, title, COLOR_TITLE);
    draw_text_centered(fb_width / 2, menu_y - 56, "Multi-Boot Selector", COLOR_TEXT_DIM);
    /* Subtitle with entry count */
    char sub[64];
    snprintf(sub, sizeof(sub), "%d boot option(s) available", entry_count);
    draw_text_centered(fb_width / 2, menu_y - 32, sub, COLOR_TEXT_DIM);
    /* Calculate scroll */
    if (selected < menu_scroll) menu_scroll = selected;
    if (selected >= menu_scroll + menu_visible_count) menu_scroll = selected - menu_visible_count + 1;
    /* Draw menu items */
    for (int i = 0; i < menu_visible_count && (menu_scroll + i) < entry_count; i++) {
        int idx = menu_scroll + i;
        int y = menu_y + i * (menu_item_height + 12);
        int is_sel = (idx == selected);
        unsigned int bg_color = is_sel ? COLOR_HIGHLIGHT : COLOR_PANEL;
        unsigned int text_color = is_sel ? COLOR_HIGHLIGHT_TEXT : COLOR_TEXT;
        /* Panel */
        fill_rect(menu_x, y, menu_item_width, menu_item_height, bg_color);
        draw_rect(menu_x, y, menu_item_width, menu_item_height,
                  is_sel ? COLOR_HIGHLIGHT_TEXT : COLOR_PANEL_BORDER);
        /* Selection indicator */
        if (is_sel) {
            fill_rect(menu_x, y, 6, menu_item_height, COLOR_TITLE);
        }
        /* Entry number */
        char num[8];
        snprintf(num, sizeof(num), "%d.", idx + 1);
        draw_text(menu_x + 20, y + (menu_item_height - 16) / 2, num,
                  is_sel ? COLOR_TITLE : COLOR_TEXT_DIM);
        /* Entry name */
        draw_text(menu_x + 60, y + (menu_item_height - 16) / 2,
                  entries[idx].name, text_color);
    }
    /* Scroll indicators */
    if (menu_scroll > 0) {
        draw_text_centered(fb_width / 2, menu_y - 8, "^ ^ ^", COLOR_TEXT_DIM);
    }
    if (menu_scroll + menu_visible_count < entry_count) {
        int bottom_y = menu_y + menu_visible_count * (menu_item_height + 12) + 4;
        draw_text_centered(fb_width / 2, bottom_y, "v v v", COLOR_TEXT_DIM);
    }
    /* Footer instructions */
    int footer_y = fb_height - 60;
    draw_text_centered(fb_width / 2, footer_y,
                       "Vol+/- : Navigate   Power/Touch : Select   Back : Recovery", COLOR_TEXT_DIM);
    draw_text_centered(fb_width / 2, footer_y + 22,
                       "Config: /data/setboot/file.txt", COLOR_TEXT_DIM);
    drm_flush();
}

static void render_flash_screen(const char *name, const char *status, int progress) {
    draw_gradient_bg();
    draw_text_centered(fb_width / 2, fb_height / 2 - 80, "Flashing Boot Image", COLOR_TITLE);    draw_text_centered(fb_width / 2, fb_height / 2 - 50, name, COLOR_TEXT);
    /* Progress bar */
    int bar_w = fb_width * 2 / 3;
    int bar_h = 32;
    int bar_x = (fb_width - bar_w) / 2;
    int bar_y = fb_height / 2;
    fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_PROGRESS_BG);
    if (progress > 0) {
        fill_rect(bar_x, bar_y, bar_w * progress / 100, bar_h, COLOR_PROGRESS_FG);
    }
    draw_rect(bar_x, bar_y, bar_w, bar_h, COLOR_TEXT_DIM);
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", progress);
    draw_text_centered(fb_width / 2, bar_y + 8, pct, COLOR_HIGHLIGHT_TEXT);
    if (status) {
        draw_text_centered(fb_width / 2, bar_y + bar_h + 20, status, COLOR_TEXT);
    }
    drm_flush();
}

static void render_error_screen(const char *msg) {
    draw_gradient_bg();
    draw_text_centered(fb_width / 2, fb_height / 2 - 40, "ERROR", COLOR_STATUS_ERR);
    draw_text_centered(fb_width / 2, fb_height / 2, msg, COLOR_TEXT);
    draw_text_centered(fb_width / 2, fb_height / 2 + 40,
                       "Press Power to continue", COLOR_TEXT_DIM);
    drm_flush();
}

/* ============ Main ============ */
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
    /* Parse config */
    if (parse_config(CONFIG_PATH) <= 0) {
        /* No config or empty - show error and continue to recovery */
        render_error_screen("No config file found at /data/setboot/file.txt");
        /* Wait for power key or timeout */
        int waited = 0;
        while (waited < 10000) {
            int tx, ty;
            int inp = wait_input(500, &tx, &ty);
            if (inp == INPUT_SELECT || inp == INPUT_TOUCH) break;
            waited += 500;
        }
        fb_close();
        close_input_devices();
        return 0;
    }
    /* Calculate layout */
    calc_menu_layout();
    selected = 0;
    /* Main menu loop */
    while (1) {
        render_menu();
        int tx = -1, ty = -1;
        int inp = wait_input(5000, &tx, &ty);
        switch (inp) {
            case INPUT_VOL_UP:
                if (selected > 0) selected--;
                else selected = entry_count - 1;
                break;
            case INPUT_VOL_DN:
                if (selected < entry_count - 1) selected++;
                else selected = 0;
                break;
            case INPUT_SELECT:
                goto do_flash;
            case INPUT_TOUCH: {
                /* Map touch coordinates to menu item */
                /* Touch may need rotation/scale correction */
                int touch_idx = -1;
                for (int i = 0; i < menu_visible_count && (menu_scroll + i) < entry_count; i++) {
                    int y = menu_y + i * (menu_item_height + 12);
                    if (ty >= y && ty <= y + menu_item_height &&
                        tx >= menu_x && tx <= menu_x + menu_item_width) {
                        touch_idx = menu_scroll + i;
                        break;
                    }
                }
                if (touch_idx >= 0) {
                    selected = touch_idx;
                    goto do_flash;
                }
                break;
            }
            case INPUT_BACK:
                /* Back key = continue to recovery without flashing */
                fb_close();
                close_input_devices();
                return 0;
            default:
                break;
        }
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
            int inp = wait_input(500, &tx, &ty);
            if (inp == INPUT_SELECT || inp == INPUT_TOUCH) break;
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
                int inp = wait_input(500, &tx, &ty);
                if (inp == INPUT_SELECT || inp == INPUT_TOUCH) break;
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
