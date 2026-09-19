/*
 * drmtest — drives /dev/dri/card0 from userspace with raw DRM ioctls.
 *
 * It exercises the in-tree DRM/KMS core end to end: the 2D KMS path (dumb
 * buffer -> framebuffer -> SETCRTC) and the feature areas the core grew later:
 * blob properties (including the connector EDID blob), cursor, universal
 * planes, the atomic API, vblank waits, syncobj and format modifiers.
 *
 * Every check prints one line of the form
 *     drmtest: <what> = <rc>
 * so a boot log can be grepped for the result.  The run ends with a summary
 * line carrying the pass/fail counts.
 *
 * The structs and ioctl numbers come straight from the kernel's own uapi
 * headers, so a layout drift between core and client shows up here.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <mman.h>

#include <drm.h>
#include <drm_mode.h>
#include <drm_fourcc.h>

#define MAX_LIST 32

static int g_pass;
static int g_fail;

#define CHECK(name, rc)                                                        \
    do {                                                                       \
        long _r = (long)(rc);                                                  \
        if (_r == 0) {                                                         \
            g_pass++;                                                          \
            printf("drmtest: %s = 0\n", name);                                 \
        } else {                                                               \
            g_fail++;                                                          \
            printf("drmtest: %s = %ld (FAIL errno=%d)\n", name, _r, errno);    \
        }                                                                      \
    } while (0)

static uint32_t fourcc_xrgb8888(void) {
    return (uint32_t)'X' | ((uint32_t)'R' << 8) | ((uint32_t)'2' << 16) |
           ((uint32_t)'4' << 24);
}

/*
 * Find one property of an object by name and return its current value and
 * flags — the two-call OBJ_GETPROPERTIES pattern libdrm uses.
 */
static int find_prop(int fd, uint32_t obj_type, uint32_t obj_id,
                     const char *want, uint64_t *value_out, uint32_t *flags_out) {
    struct drm_mode_obj_get_properties gp;
    uint32_t *ids;
    uint64_t *vals;
    uint32_t count, i;
    int found = -1;

    memset(&gp, 0, sizeof(gp));
    gp.obj_type = obj_type;
    gp.obj_id = obj_id;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) != 0)
        return -1;
    count = gp.count_props;
    if (count == 0)
        return -1;

    ids = (uint32_t *)malloc(count * sizeof(uint32_t));
    vals = (uint64_t *)malloc(count * sizeof(uint64_t));
    if (!ids || !vals) {
        free(ids);
        free(vals);
        return -1;
    }
    memset(ids, 0, count * sizeof(uint32_t));
    memset(vals, 0, count * sizeof(uint64_t));

    memset(&gp, 0, sizeof(gp));
    gp.obj_type = obj_type;
    gp.obj_id = obj_id;
    gp.props_ptr = (uint64_t)(uintptr_t)ids;
    gp.prop_values_ptr = (uint64_t)(uintptr_t)vals;
    gp.count_props = count;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) != 0)
        goto out;

    for (i = 0; i < count; i++) {
        struct drm_mode_get_property pr;
        memset(&pr, 0, sizeof(pr));
        pr.prop_id = ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &pr) != 0)
            continue;
        if (strcmp(pr.name, want) == 0) {
            if (value_out)
                *value_out = vals[i];
            if (flags_out)
                *flags_out = pr.flags;
            found = (int)ids[i];
            break;
        }
    }

out:
    free(ids);
    free(vals);
    return found;
}

/* ── blob properties ────────────────────────────────────────────────────── */

static int test_blobs(int fd, uint32_t conn_id, uint32_t crtc_id) {
    struct drm_mode_create_blob cb;
    struct drm_mode_get_blob gb;
    struct drm_mode_destroy_blob db;
    uint64_t edid_value = 0;
    uint32_t edid_flags = 0;
    int prop;
    uint8_t payload[16];
    uint8_t back[64];
    uint8_t edid[2048];
    int rc;

    /* Step 1 — OBJ_GETPROPERTIES(connector): find the property called "EDID";
     * its value is the blob id. */
    prop = find_prop(fd, DRM_MODE_OBJECT_CONNECTOR, conn_id, "EDID",
                     &edid_value, &edid_flags);
    printf("drmtest: step1 OBJ_GETPROPERTIES(conn) name=EDID prop=%d blob=%u flags=0x%x\n",
           prop, (unsigned)edid_value, (unsigned)edid_flags);

    /* Step 2 — GETPROPERTY(prop_id): confirm it is a BLOB property.  For blob
     * properties the ABI reports count_values as the number of value slots
     * (0 for EDID on Linux, because the payload length lives with the blob),
     * so the byte length comes from GETPROPBLOB below. */
    if (prop > 0) {
        struct drm_mode_get_property pr;
        memset(&pr, 0, sizeof(pr));
        pr.prop_id = (uint32_t)prop;
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &pr) == 0)
            printf("drmtest: step2 GETPROPERTY name='%s' is_blob=%d flags=0x%x count_values=%u\n",
                   pr.name, (pr.flags & DRM_MODE_PROP_BLOB) ? 1 : 0,
                   (unsigned)pr.flags, (unsigned)pr.count_values);
        else {
            g_fail++;
            printf("drmtest: step2 GETPROPERTY FAIL errno=%d\n", errno);
        }
    }

    /* Step 3 — GETPROPBLOB(blob_id): ask for the length, then the bytes. */
    if (prop > 0 && (edid_flags & DRM_MODE_PROP_BLOB) && edid_value != 0) {
        memset(&gb, 0, sizeof(gb));
        gb.blob_id = (uint32_t)edid_value;
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &gb) == 0) {
            uint32_t len = gb.length;
            printf("drmtest: step3 GETPROPBLOB length=%u blocks=%u\n",
                   (unsigned)len, (unsigned)(len / 128));
            if (len > 0 && len <= sizeof(edid)) {
                memset(&gb, 0, sizeof(gb));
                gb.blob_id = (uint32_t)edid_value;
                gb.length = len;
                gb.data = (uint64_t)(uintptr_t)edid;
                memset(edid, 0, sizeof(edid));
                rc = ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &gb);
                CHECK("EDID GETPROPBLOB", rc);
                /* The EDID declares its own length: 128 * (1 + edid[126]). */
                printf("drmtest: EDID header=%02x%02x mfg=%02x%02x ext_count=%u declared_len=%u got=%u\n",
                       edid[0], edid[1], edid[8], edid[9], (unsigned)edid[126],
                       (unsigned)((1u + edid[126]) * 128u), (unsigned)gb.length);
            } else {
                g_fail++;
                printf("drmtest: EDID blob length %u out of range (FAIL)\n",
                       (unsigned)len);
            }
        } else {
            g_fail++;
            printf("drmtest: EDID GETPROPBLOB (length query) FAIL errno=%d\n",
                   errno);
        }
    } else {
        g_fail++;
        printf("drmtest: EDID blob property missing (FAIL)\n");
    }

    /* CREATE_BLOB / GET_BLOB / DESTROY_BLOB round trip. */
    for (rc = 0; rc < 16; rc++)
        payload[rc] = (uint8_t)(0xa0 + rc);

    memset(&cb, 0, sizeof(cb));
    cb.data = (uint64_t)(uintptr_t)payload;
    cb.length = sizeof(payload);
    rc = ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &cb);
    CHECK("MODE_CREATEPROPBLOB", rc);
    if (rc != 0) {
        printf("drmtest: (crtc %u)\n", (unsigned)crtc_id);
        return -1;
    }
    printf("drmtest: CREATEPROPBLOB blob=%u\n", (unsigned)cb.blob_id);

    memset(back, 0, sizeof(back));
    memset(&gb, 0, sizeof(gb));
    gb.blob_id = cb.blob_id;
    gb.length = sizeof(payload);
    gb.data = (uint64_t)(uintptr_t)back;
    rc = ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &gb);
    CHECK("MODE_GETPROPBLOB", rc);
    printf("drmtest: GETPROPBLOB length=%u data_match=%d\n",
           (unsigned)gb.length, memcmp(back, payload, sizeof(payload)) == 0);

    memset(&db, 0, sizeof(db));
    db.blob_id = cb.blob_id;
    rc = ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &db);
    CHECK("MODE_DESTROYPROPBLOB", rc);

    return 0;
}

/* ── cursor ─────────────────────────────────────────────────────────────── */

static void test_cursor(int fd, uint32_t crtc_id, uint32_t handle) {
    struct drm_mode_cursor cur;
    struct drm_mode_cursor2 cur2;

    memset(&cur, 0, sizeof(cur));
    cur.flags = DRM_MODE_CURSOR_BO;
    cur.crtc_id = crtc_id;
    cur.width = 64;
    cur.height = 64;
    cur.handle = handle;
    CHECK("MODE_CURSOR", ioctl(fd, DRM_IOCTL_MODE_CURSOR, &cur));

    memset(&cur2, 0, sizeof(cur2));
    cur2.flags = DRM_MODE_CURSOR_MOVE;
    cur2.crtc_id = crtc_id;
    cur2.x = 32;
    cur2.y = 24;
    CHECK("MODE_CURSOR2", ioctl(fd, DRM_IOCTL_MODE_CURSOR2, &cur2));
}

/* ── planes ─────────────────────────────────────────────────────────────── */

static void test_planes(int fd, uint32_t crtc_id, uint32_t fb_id, uint32_t w,
                        uint32_t h) {
    struct drm_mode_get_plane_res pr;
    struct drm_mode_get_plane gp;
    struct drm_mode_set_plane sp;
    uint32_t planes[MAX_LIST];
    int rc;

    memset(planes, 0, sizeof(planes));
    memset(&pr, 0, sizeof(pr));
    pr.plane_id_ptr = (uint64_t)(uintptr_t)planes;
    pr.count_planes = MAX_LIST;
    rc = ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
    printf("drmtest: GETPLANERESOURCES rc=%d planes=%u\n", rc,
           (unsigned)pr.count_planes);
    if (rc != 0 || pr.count_planes == 0) {
        g_fail++;
        printf("drmtest: no planes registered (FAIL)\n");
        return;
    }
    printf("drmtest: plane0 id=%u\n", (unsigned)planes[0]);

    memset(&sp, 0, sizeof(sp));
    sp.plane_id = planes[0];
    sp.crtc_id = crtc_id;
    sp.fb_id = fb_id;
    sp.crtc_w = w;
    sp.crtc_h = h;
    sp.src_w = w << 16;
    sp.src_h = h << 16;
    CHECK("MODE_SETPLANE", ioctl(fd, DRM_IOCTL_MODE_SETPLANE, &sp));

    memset(&gp, 0, sizeof(gp));
    gp.plane_id = planes[0];
    rc = ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp);
    printf("drmtest: GETPLANE plane=%u crtc=%u fb=%u formats=%u rc=%d\n",
           (unsigned)gp.plane_id, (unsigned)gp.crtc_id, (unsigned)gp.fb_id,
           (unsigned)gp.count_format_types, rc);
}

/* ── atomic ─────────────────────────────────────────────────────────────── */

static void test_atomic(int fd, uint32_t crtc_id, uint32_t conn_id,
                        uint32_t fb_id, uint32_t w, uint32_t h) {
    struct drm_mode_create_blob cb;
    struct drm_mode_modeinfo mi;
    struct drm_mode_atomic at;
    uint32_t objs[2];
    uint32_t counts[2];
    uint32_t props[3];
    uint64_t vals[3];
    uint64_t mode_blob;
    int prop_mode, prop_active, prop_crtc;
    int rc;

    /* MODE_ID blob: the mode the commit should install. */
    memset(&mi, 0, sizeof(mi));
    mi.clock = 73008;
    mi.hdisplay = (uint16_t)w;
    mi.hsync_start = (uint16_t)(w + 32);
    mi.hsync_end = (uint16_t)(w + 96);
    mi.htotal = (uint16_t)(w + 160);
    mi.vdisplay = (uint16_t)h;
    mi.vsync_start = (uint16_t)(h + 3);
    mi.vsync_end = (uint16_t)(h + 8);
    mi.vtotal = (uint16_t)(h + 13);
    mi.vrefresh = 60;
    mi.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    strcpy(mi.name, "1280x800");

    memset(&cb, 0, sizeof(cb));
    cb.data = (uint64_t)(uintptr_t)&mi;
    cb.length = sizeof(mi);
    if (ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &cb) != 0) {
        g_fail++;
        printf("drmtest: atomic MODE_ID blob FAIL errno=%d\n", errno);
        return;
    }
    mode_blob = cb.blob_id;

    prop_mode = find_prop(fd, DRM_MODE_OBJECT_CRTC, crtc_id, "MODE_ID", NULL, NULL);
    prop_active = find_prop(fd, DRM_MODE_OBJECT_CRTC, crtc_id, "ACTIVE", NULL, NULL);
    prop_crtc = find_prop(fd, DRM_MODE_OBJECT_CONNECTOR, conn_id, "CRTC_ID", NULL, NULL);
    if (prop_mode <= 0 || prop_active <= 0 || prop_crtc <= 0) {
        g_fail++;
        printf("drmtest: atomic props missing (MODE_ID=%d ACTIVE=%d CRTC_ID=%d) FAIL\n",
               prop_mode, prop_active, prop_crtc);
        return;
    }

    /* The commit names two objects: the CRTC (MODE_ID + ACTIVE) and the
     * connector (CRTC_ID). */
    objs[0] = crtc_id;
    objs[1] = conn_id;
    counts[0] = 2;
    counts[1] = 1;
    props[0] = (uint32_t)prop_mode;
    vals[0] = mode_blob;
    props[1] = (uint32_t)prop_active;
    vals[1] = 1;
    props[2] = (uint32_t)prop_crtc;
    vals[2] = crtc_id;

    (void)fb_id;

    memset(&at, 0, sizeof(at));
    at.count_objs = 2;
    at.objs_ptr = (uint64_t)(uintptr_t)objs;
    at.count_props_ptr = (uint64_t)(uintptr_t)counts;
    at.props_ptr = (uint64_t)(uintptr_t)props;
    at.prop_values_ptr = (uint64_t)(uintptr_t)vals;
    at.flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
    rc = ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &at);
    CHECK("MODE_ATOMIC(TEST_ONLY)", rc);

    memset(&at, 0, sizeof(at));
    at.count_objs = 2;
    at.objs_ptr = (uint64_t)(uintptr_t)objs;
    at.count_props_ptr = (uint64_t)(uintptr_t)counts;
    at.props_ptr = (uint64_t)(uintptr_t)props;
    at.prop_values_ptr = (uint64_t)(uintptr_t)vals;
    at.flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    rc = ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &at);
    CHECK("MODE_ATOMIC(commit)", rc);

    /* The committed state has to be visible through the legacy GETCRTC. */
    {
        struct drm_mode_crtc gc;
        memset(&gc, 0, sizeof(gc));
        gc.crtc_id = crtc_id;
        rc = ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &gc);
        printf("drmtest: ATOMIC GETCRTC rc=%d fb=%u mode_valid=%u %ux%u\n", rc,
               (unsigned)gc.fb_id, (unsigned)gc.mode_valid,
               (unsigned)gc.mode.hdisplay, (unsigned)gc.mode.vdisplay);
        if (rc != 0 || gc.mode_valid == 0)
            g_fail++;
    }
}

/* ── vblank ─────────────────────────────────────────────────────────────── */

static void test_vblank(int fd, uint32_t crtc_id) {
    union drm_wait_vblank wv;
    uint32_t now;
    uint32_t target;
    int rc;

    memset(&wv, 0, sizeof(wv));
    wv.request.type = _DRM_VBLANK_ABSOLUTE;
    wv.request.sequence = 0;
    if (ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &wv) != 0) {
        g_fail++;
        printf("drmtest: WAIT_VBLANK(read) FAIL errno=%d\n", errno);
        return;
    }
    now = wv.reply.sequence;
    target = now + 2;
    printf("drmtest: vblank sequence=%u target=%u\n", (unsigned)now,
           (unsigned)target);

    memset(&wv, 0, sizeof(wv));
    wv.request.type = _DRM_VBLANK_ABSOLUTE | _DRM_VBLANK_NEXTONMISS;
    wv.request.sequence = target;
    rc = ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &wv);
    CHECK("WAIT_VBLANK(future)", rc);
    printf("drmtest: WAIT_VBLANK got=%u target_met=%d crtc=%u\n",
           (unsigned)wv.reply.sequence, wv.reply.sequence >= target,
           (unsigned)crtc_id);
}

/* ── syncobj ────────────────────────────────────────────────────────────── */

static void test_syncobj(int fd) {
    struct drm_syncobj_create cr;
    struct drm_syncobj_destroy de;
    struct drm_syncobj_handle hs;
    struct drm_syncobj_array arr;
    uint32_t handles[1];
    int rc;

    memset(&cr, 0, sizeof(cr));
    rc = ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &cr);
    CHECK("SYNCOBJ_CREATE", rc);
    if (rc != 0)
        return;
    printf("drmtest: SYNCOBJ_CREATE handle=%u\n", (unsigned)cr.handle);

    memset(&hs, 0, sizeof(hs));
    hs.handle = cr.handle;
    hs.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    rc = ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &hs);
    CHECK("SYNCOBJ_HANDLE_TO_FD", rc);

    if (rc == 0) {
        struct drm_syncobj_handle imp;
        memset(&imp, 0, sizeof(imp));
        imp.fd = hs.fd;
        imp.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
        CHECK("SYNCOBJ_FD_TO_HANDLE",
              ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &imp));
        if (hs.fd >= 0)
            close(hs.fd);
    }

    handles[0] = cr.handle;
    memset(&arr, 0, sizeof(arr));
    arr.handles = (uint64_t)(uintptr_t)handles;
    arr.count_handles = 1;
    CHECK("SYNCOBJ_SIGNAL", ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &arr));

    /* Wait must return immediately now that it is signalled. */
    {
        struct drm_syncobj_wait w;
        memset(&w, 0, sizeof(w));
        w.handles = (uint64_t)(uintptr_t)handles;
        w.count_handles = 1;
        w.timeout_nsec = 1000000; /* 1 ms */
        CHECK("SYNCOBJ_WAIT", ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w));
    }

    memset(&de, 0, sizeof(de));
    de.handle = cr.handle;
    CHECK("SYNCOBJ_DESTROY", ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &de));

    /* The timeline flavour, so DRM_CAP_SYNCOBJ_TIMELINE is not a hollow claim:
     * signal a point, wait for it, then read it back. */
    {
        struct drm_syncobj_create tc;
        struct drm_syncobj_destroy td;
        struct drm_syncobj_timeline_array ta;
        struct drm_syncobj_timeline_wait tw;
        uint32_t th[1];
        uint64_t tpts[1];

        memset(&tc, 0, sizeof(tc));
        tc.flags = 1u << 1; /* DRM_SYNCOBJ_CREATE_TYPE_TIMELINE */
        if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &tc) != 0) {
            g_fail++;
            printf("drmtest: SYNCOBJ timeline create FAIL errno=%d\n", errno);
            return;
        }
        th[0] = tc.handle;

        tpts[0] = 1;
        memset(&ta, 0, sizeof(ta));
        ta.handles = (uint64_t)(uintptr_t)th;
        ta.points = (uint64_t)(uintptr_t)tpts;
        ta.count_handles = 1;
        CHECK("SYNCOBJ_TIMELINE_SIGNAL",
              ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &ta));

        memset(&tw, 0, sizeof(tw));
        tw.handles = (uint64_t)(uintptr_t)th;
        tw.points = (uint64_t)(uintptr_t)tpts;
        tw.count_handles = 1;
        tw.timeout_nsec = 1000000;
        CHECK("SYNCOBJ_TIMELINE_WAIT", ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &tw));

        tpts[0] = 0;
        memset(&ta, 0, sizeof(ta));
        ta.handles = (uint64_t)(uintptr_t)th;
        ta.points = (uint64_t)(uintptr_t)tpts;
        ta.count_handles = 1;
        rc = ioctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &ta);
        printf("drmtest: SYNCOBJ_QUERY rc=%d point=%llu\n", rc,
               (unsigned long long)tpts[0]);

        memset(&td, 0, sizeof(td));
        td.handle = tc.handle;
        ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &td);
    }
}

/* ── modifiers ──────────────────────────────────────────────────────────── */

static void test_modifiers(int fd, uint32_t handle, uint32_t w, uint32_t h,
                           uint32_t pitch) {
    struct drm_get_cap cap;
    struct drm_mode_fb_cmd2 f2;
    int rc;

    memset(&cap, 0, sizeof(cap));
    cap.capability = DRM_CAP_ADDFB2_MODIFIERS;
    rc = ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
    printf("drmtest: CAP_ADDFB2_MODIFIERS rc=%d value=%llu\n", rc,
           (unsigned long long)cap.value);

    memset(&f2, 0, sizeof(f2));
    f2.width = w;
    f2.height = h;
    f2.pixel_format = fourcc_xrgb8888();
    f2.handles[0] = handle;
    f2.pitches[0] = pitch;
    f2.offsets[0] = 0;
    f2.modifier[0] = DRM_FORMAT_MOD_LINEAR;
    CHECK("ADDFB2(modifier=LINEAR)", ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f2));
    if (rc == 0)
        printf("drmtest: ADDFB2(modifier) fb_id=%u\n", (unsigned)f2.fb_id);
}

/* Printed for `drmtest --help`. */
static const char drmtest_usage[] =
    "usage: drmtest [--help]\n"
    "\n"
    "Smoke-test the CactOS DRM/KMS core by driving /dev/dri/card0 with raw DRM\n"
    "ioctls: KMS resources, connectors and modes, a dumb buffer that is mapped\n"
    "and turned into a framebuffer, blob properties (including the connector\n"
    "EDID blob), cursor, universal planes, the atomic API, vblank waits,\n"
    "syncobj and format modifiers.\n"
    "\n"
    "Every check prints a line 'drmtest: <what> = <rc>' and the run ends with a\n"
    "pass/fail summary.  Exit status is 0 when every check passed, non-zero\n"
    "otherwise.\n"
    "\n"
    "  --help      print this help and exit\n";

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    struct drm_mode_card_res res;
    struct drm_mode_get_connector gc;
    struct drm_mode_create_dumb dumb;
    struct drm_mode_map_dumb map;
    struct drm_mode_fb_cmd2 afb;
    struct drm_mode_crtc set;
    struct drm_mode_crtc get;
    struct drm_mode_modeinfo modes[MAX_LIST];
    uint32_t crtcs[MAX_LIST], conns[MAX_LIST], encs[MAX_LIST];
    void *mapped;
    int fd;
    int rc;

    if (argc > 1 && !strcmp(argv[1], "--help")) {
        printf("%s", drmtest_usage);
        return 0;
    }

    fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) {
        printf("drmtest: open /dev/dri/card0 = %d (FAIL errno=%d)\n", fd, errno);
        printf("drmtest: done\n");
        return 1;
    }
    printf("drmtest: open /dev/dri/card0 = %d\n", fd);

    /* ── resources ─────────────────────────────────────────────────────── */
    memset(crtcs, 0, sizeof(crtcs));
    memset(conns, 0, sizeof(conns));
    memset(encs, 0, sizeof(encs));
    memset(&res, 0, sizeof(res));
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encs;
    res.count_crtcs = MAX_LIST;
    res.count_connectors = MAX_LIST;
    res.count_encoders = MAX_LIST;
    CHECK("GETRESOURCES", ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res));
    printf("drmtest: GETRESOURCES crtc=%u conn=%u enc=%u fb=%u\n",
           (unsigned)res.count_crtcs, (unsigned)res.count_connectors,
           (unsigned)res.count_encoders, (unsigned)res.count_fbs);
    if (res.count_crtcs == 0 || res.count_connectors == 0)
        goto done;

    /* ── connector ─────────────────────────────────────────────────────── */
    memset(modes, 0, sizeof(modes));
    memset(&gc, 0, sizeof(gc));
    gc.connector_id = conns[0];
    gc.modes_ptr = (uint64_t)(uintptr_t)modes;
    gc.count_modes = MAX_LIST;
    CHECK("GETCONNECTOR", ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc));
    printf("drmtest: GETCONNECTOR type=%u type_id=%u status=%u modes=%u\n",
           (unsigned)gc.connector_type, (unsigned)gc.connector_type_id,
           (unsigned)gc.connection, (unsigned)gc.count_modes);
    printf("drmtest: mode0 clock=%u %ux%u\n", (unsigned)modes[0].clock,
           (unsigned)modes[0].hdisplay, (unsigned)modes[0].vdisplay);

    /* ── dumb buffer + framebuffer ─────────────────────────────────────── */
    memset(&dumb, 0, sizeof(dumb));
    dumb.width = 1280;
    dumb.height = 800;
    dumb.bpp = 32;
    rc = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
    CHECK("CREATE_DUMB", rc);
    if (rc != 0)
        goto done;
    printf("drmtest: CREATE_DUMB handle=%u pitch=%u size=%llu\n",
           (unsigned)dumb.handle, (unsigned)dumb.pitch,
           (unsigned long long)dumb.size);

    memset(&map, 0, sizeof(map));
    map.handle = dumb.handle;
    rc = ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    CHECK("MAP_DUMB", rc);
    if (rc != 0)
        goto done;
    printf("drmtest: MAP_DUMB offset=0x%llx\n", (unsigned long long)map.offset);

    mapped = mmap(NULL, (size_t)dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  fd, (unsigned int)map.offset);
    printf("drmtest: mmap = %p\n", mapped);
    if (mapped != MAP_FAILED) {
        memset(mapped, 0x5a, (size_t)dumb.size);
        printf("drmtest: wrote 0x5a over %u bytes\n", (unsigned)dumb.size);
    } else {
        g_fail++;
        printf("drmtest: mmap FAIL errno=%d\n", errno);
        goto done;
    }

    memset(&afb, 0, sizeof(afb));
    afb.width = dumb.width;
    afb.height = dumb.height;
    afb.pixel_format = fourcc_xrgb8888();
    afb.handles[0] = dumb.handle;
    afb.pitches[0] = dumb.pitch;
    afb.offsets[0] = 0;
    rc = ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &afb);
    CHECK("ADDFB2", rc);
    if (rc != 0)
        goto done;
    printf("drmtest: ADDFB2 fb_id=%u\n", (unsigned)afb.fb_id);

    /* ── blobs ─────────────────────────────────────────────────────────── */
    test_blobs(fd, conns[0], crtcs[0]);

    /* ── cursor ────────────────────────────────────────────────────────── */
    {
        /* The cursor image is its own small buffer, as a compositor keeps it. */
        struct drm_mode_create_dumb cdumb;
        memset(&cdumb, 0, sizeof(cdumb));
        cdumb.width = 64;
        cdumb.height = 64;
        cdumb.bpp = 32;
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cdumb) == 0)
            printf("drmtest: cursor buffer handle=%u pitch=%u\n",
                   (unsigned)cdumb.handle, (unsigned)cdumb.pitch);
        else
            printf("drmtest: cursor buffer FAIL errno=%d\n", errno);
        test_cursor(fd, crtcs[0], cdumb.handle);
    }

    /* ── universal planes ──────────────────────────────────────────────── */
    {
        struct drm_set_client_cap sc;
        memset(&sc, 0, sizeof(sc));
        sc.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
        sc.value = 1;
        CHECK("SET_CLIENT_CAP(UNIVERSAL_PLANES)",
              ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &sc));
    }
    test_planes(fd, crtcs[0], afb.fb_id, dumb.width, dumb.height);

    /* ── atomic ────────────────────────────────────────────────────────── */
    {
        struct drm_set_client_cap sc;
        memset(&sc, 0, sizeof(sc));
        sc.capability = DRM_CLIENT_CAP_ATOMIC;
        sc.value = 1;
        CHECK("SET_CLIENT_CAP(ATOMIC)",
              ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &sc));
    }
    test_atomic(fd, crtcs[0], conns[0], afb.fb_id, dumb.width, dumb.height);

    /* ── vblank ────────────────────────────────────────────────────────── */
    test_vblank(fd, crtcs[0]);

    /* ── syncobj ───────────────────────────────────────────────────────── */
    {
        struct drm_get_cap cap;
        memset(&cap, 0, sizeof(cap));
        cap.capability = DRM_CAP_SYNCOBJ;
        ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
        printf("drmtest: CAP_SYNCOBJ value=%llu\n",
               (unsigned long long)cap.value);
        memset(&cap, 0, sizeof(cap));
        cap.capability = DRM_CAP_SYNCOBJ_TIMELINE;
        ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
        printf("drmtest: CAP_SYNCOBJ_TIMELINE value=%llu\n",
               (unsigned long long)cap.value);
    }
    test_syncobj(fd);

    /* ── modifiers ─────────────────────────────────────────────────────── */
    test_modifiers(fd, dumb.handle, dumb.width, dumb.height, dumb.pitch);

    /* ── the 2D path: SETCRTC ──────────────────────────────────────────── */
    memset(&set, 0, sizeof(set));
    set.crtc_id = crtcs[0];
    set.fb_id = afb.fb_id;
    set.mode_valid = 1;
    set.mode = modes[0];
    set.count_connectors = 1;
    set.set_connectors_ptr = (uint64_t)(uintptr_t)&conns[0];
    rc = ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set);
    CHECK("SETCRTC", rc);

    memset(&get, 0, sizeof(get));
    get.crtc_id = crtcs[0];
    rc = ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &get);
    printf("drmtest: GETCRTC rc=%d fb=%u mode_valid=%u %ux%u\n", rc,
           (unsigned)get.fb_id, (unsigned)get.mode_valid,
           (unsigned)get.mode.hdisplay, (unsigned)get.mode.vdisplay);

done:
    printf("drmtest: summary pass=%d fail=%d\n", g_pass, g_fail);
    printf("drmtest: done\n");
    close(fd);
    return g_fail ? 1 : 0;
}
