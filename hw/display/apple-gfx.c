/*
 * QEMU Apple paravirtualized graphics device
 *
 * Copyright © 2023 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * macOS guests drive a paravirtualized GPU, the device of Apple's
 * ParavirtualizedGraphics.framework, which implements 3d graphics passthrough
 * to the host as well as a proprietary guest communication channel. This
 * device model drives reims-vgpu, a library that implements the host side of
 * that device, from within QEMU.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qapi/visitor.h"
#include "qapi/error.h"
#include "qemu/aio-wait.h"
#include "system/address-spaces.h"
#include "migration/blocker.h"
#include "system/memory.h"
#include "ui/console.h"
#include "apple-gfx.h"
#include "trace.h"

static const AppleGFXDisplayMode apple_gfx_default_modes[] = {
    { 1920, 1080, 60 },
    { 1440, 1080, 60 },
    { 1280, 1024, 60 },
};

static Error *apple_gfx_mig_blocker;
static uint32_t next_pgdisplay_serial_num = 1;

/* ------ Rendering and frame management ------ */

/*
 * One frame: the display's current frame drawn into the frame target the
 * surface had when the render started, then copied into the surface.
 */
typedef struct AppleGFXRenderJob {
    AppleGFXState *s;
    RvgFrameTarget *target;
    bool encoded;
} AppleGFXRenderJob;

static void apple_gfx_render_frame_completed_bh(void *opaque);

static int apple_gfx_do_render(void *opaque)
{
    AppleGFXRenderJob *job = opaque;
    RvgFrame *frame;
    RvgStatus status;

    /*
     * This runs on a pool thread, not under the BQL: the encode takes the
     * display's lock, which a guest present holds while it may wait for the
     * main loop.
     */
    status = rvg_display_encode_frame(job->s->pgdisp, job->target, NULL,
                                      &frame);
    job->encoded = status == RVG_OK;
    if (job->encoded) {
        rvg_frame_commit(frame);
        rvg_frame_wait(frame);
        rvg_frame_release(frame);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: rvg_display_encode_frame: %s\n",
                      __func__, rvg_status_message(status));
    }

    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            apple_gfx_render_frame_completed_bh, job);
    return 0;
}

static void apple_gfx_render_new_frame(AppleGFXState *s)
{
    AppleGFXRenderJob *job = g_new0(AppleGFXRenderJob, 1);

    assert(bql_locked());
    job->s = s;
    /* NULL where set_mode's target was refused: the encode then refuses. */
    job->target = s->target ? rvg_frame_target_retain(s->target) : NULL;

    s->rendering_frame_width = surface_width(s->surface);
    s->rendering_frame_height = surface_height(s->surface);

    thread_pool_submit_immediate(s->pool, apple_gfx_do_render, job, NULL);
}

static void apple_gfx_render_frame_completed_bh(void *opaque)
{
    AppleGFXRenderJob *job = opaque;
    AppleGFXState *s = job->s;
    DisplaySurface *surface = s->surface;

    --s->pending_frames;
    assert(s->pending_frames >= 0);

    /* Only update display if mode hasn't changed since we started rendering. */
    if (job->encoded &&
        s->rendering_frame_width == surface_width(surface) &&
        s->rendering_frame_height == surface_height(surface)) {
        /* The target has the surface's size and pixman's x8r8g8b8 layout. */
        rvg_frame_target_read_pixels(job->target, surface_data(surface),
                                     surface_stride(surface),
                                     (size_t)surface_stride(surface) *
                                     surface_height(surface));
        if (s->gfx_update_requested) {
            s->gfx_update_requested = false;
            qemu_console_update_full(s->con);
            qemu_console_hw_update_done(s->con);
            s->new_frame_ready = false;
        } else {
            s->new_frame_ready = true;
        }
    }
    rvg_frame_target_release(job->target);
    g_free(job);

    if (s->pending_frames > 0) {
        apple_gfx_render_new_frame(s);
    }
}

static void new_frame_handler_bh(void *opaque)
{
    AppleGFXState *s = opaque;

    /* Drop frames if guest gets too far ahead. */
    if (s->pending_frames >= 2) {
        return;
    }
    ++s->pending_frames;
    if (s->pending_frames > 1) {
        return;
    }

    apple_gfx_render_new_frame(s);
}

static bool apple_gfx_fb_update_display(void *opaque)
{
    AppleGFXState *s = opaque;
    bool done = true;

    assert(bql_locked());
    if (s->new_frame_ready) {
        qemu_console_update_full(s->con);
        s->new_frame_ready = false;
    } else if (s->pending_frames > 0) {
        s->gfx_update_requested = true;
        done = false;
    }

    return done;
}

static const GraphicHwOps apple_gfx_fb_ops = {
    .gfx_update = apple_gfx_fb_update_display,
};

/* ------ Mouse cursor and display mode setting ------ */

static void set_mode(AppleGFXState *s, uint32_t width, uint32_t height)
{
    RvgFrameTargetConfig target_config = {
        .struct_size = sizeof(target_config),
        .width = width,
        .height = height,
        /* The byte order of the surface's pixman x8r8g8b8. */
        .format = RVG_PIXEL_FORMAT_BGRA8_UNORM,
    };
    RvgStatus status;

    if (s->surface &&
        width == surface_width(s->surface) &&
        height == surface_height(s->surface)) {
        return;
    }

    /* A render under way keeps its own reference. */
    rvg_frame_target_release(s->target);
    s->target = NULL;

    s->surface = qemu_create_displaysurface(width, height);

    status = rvg_frame_target_create(s->pgdev, &target_config, &s->target);
    if (status != RVG_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: rvg_frame_target_create(%" PRIu32 "x%" PRIu32
                      "): %s\n", __func__, width, height,
                      rvg_status_message(status));
    }

    qemu_console_set_surface(s->con, s->surface);
}

static void update_cursor(AppleGFXState *s)
{
    RvgDisplayCoord position;

    assert(bql_locked());
    position = rvg_display_cursor_position(s->pgdisp);
    qemu_console_set_mouse(s->con, position.x, position.y,
                           qatomic_read(&s->cursor_show));
}

static void update_cursor_bh(void *opaque)
{
    AppleGFXState *s = opaque;
    update_cursor(s);
}

typedef struct AppleGFXSetCursorGlyphJob {
    AppleGFXState *s;
    /* A copy of the RvgCursorGlyph, which is borrowed for its callback. */
    uint8_t *rgba;
    uint32_t width;
    uint32_t height;
    RvgDisplayCoord hot_spot;
} AppleGFXSetCursorGlyphJob;

static void set_cursor_glyph(void *opaque)
{
    AppleGFXSetCursorGlyphJob *job = opaque;
    AppleGFXState *s = job->s;
    const uint8_t *px_data = job->rgba;
    uint32_t *dest_px;

    /* An RvgCursorGlyph has 32 bits a pixel, and width * 4 bytes a row. */
    trace_apple_gfx_cursor_set(32, job->width, job->height);

    if (s->cursor) {
        cursor_unref(s->cursor);
        s->cursor = NULL;
    }

    s->cursor = cursor_alloc(job->width, job->height);
    s->cursor->hot_x = job->hot_spot.x;
    s->cursor->hot_y = job->hot_spot.y;

    dest_px = s->cursor->data;
    for (uint32_t y = 0; y < job->height; ++y) {
        for (uint32_t x = 0; x < job->width; ++x) {
            /*
             * RvgCursorGlyph's red & blue channels are swapped compared to
             * QEMUCursor's.
             */
            *dest_px =
                (px_data[0] << 16u) |
                (px_data[1] <<  8u) |
                (px_data[2] <<  0u) |
                (px_data[3] << 24u);
            ++dest_px;
            px_data += 4;
        }
    }
    qemu_console_set_cursor(s->con, s->cursor);
    update_cursor(s);

    g_free(job->rgba);
    g_free(job);
}

/* ------ Memory-mapped device I/O operations ------ */

typedef struct AppleGFXIOJob {
    AppleGFXState *state;
    uint64_t offset;
    uint64_t value;
    bool completed;
} AppleGFXIOJob;

static int apple_gfx_do_read(void *opaque)
{
    AppleGFXIOJob *job = opaque;
    job->value = rvg_device_mmio_read(job->state->pgdev, job->offset);
    qatomic_set(&job->completed, true);
    aio_wait_kick();
    return 0;
}

static uint64_t apple_gfx_read(void *opaque, hwaddr offset, unsigned size)
{
    AppleGFXState *s = opaque;
    AppleGFXIOJob job = {
        .state = s,
        .offset = offset,
        .completed = false,
    };

    thread_pool_submit_immediate(s->pool, apple_gfx_do_read, &job, NULL);
    AIO_WAIT_WHILE(NULL, !qatomic_read(&job.completed));

    trace_apple_gfx_read(offset, job.value);
    return job.value;
}

static int apple_gfx_do_write(void *opaque)
{
    AppleGFXIOJob *job = opaque;
    rvg_device_mmio_write(job->state->pgdev, job->offset, job->value);
    qatomic_set(&job->completed, true);
    aio_wait_kick();
    return 0;
}

static void apple_gfx_write(void *opaque, hwaddr offset, uint64_t val,
                            unsigned size)
{
    /*
     * rvg_device_mmio_read and especially rvg_device_mmio_write may block on
     * the library's own threads, and may call back. For this reason, and as
     * we are holding the BQL, we make the calls on a pool thread, which
     * thread_pool_submit_immediate starts if none is free, and handle AIO
     * tasks while we wait. Any work in the callbacks requiring the BQL will
     * in turn schedule BHs which this thread will process while waiting.
     */
    AppleGFXState *s = opaque;
    AppleGFXIOJob job = {
        .state = s,
        .offset = offset,
        .value = val,
        .completed = false,
    };

    thread_pool_submit_immediate(s->pool, apple_gfx_do_write, &job, NULL);
    AIO_WAIT_WHILE(NULL, !qatomic_read(&job.completed));

    trace_apple_gfx_write(offset, val);
}

static const MemoryRegionOps apple_gfx_ops = {
    .read = apple_gfx_read,
    .write = apple_gfx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* ------ Initialisation and startup ------ */

void apple_gfx_common_init(Object *obj, AppleGFXState *s, const char* obj_name)
{
    trace_apple_gfx_common_init(obj_name, RVG_DEVICE_MMIO_SIZE);
    memory_region_init_io(&s->iomem_gfx, obj, &apple_gfx_ops, s, obj_name,
                          RVG_DEVICE_MMIO_SIZE);

    /*
     * TODO: reims-vgpu can suspend and resume the device
     * (rvg_device_will_suspend and the rest): integrate it!
     */
}

static bool apple_gfx_add_memory_range(Int128 start, Int128 len,
                                       const MemoryRegion *mr,
                                       hwaddr offset_in_region, void *opaque)
{
    GArray *ranges = opaque;

    if (memory_access_is_direct(mr, true, MEMTXATTRS_UNSPECIFIED)) {
        RvgMemoryRange range = {
            .physical_address = int128_get64(start),
            .physical_length = int128_get64(len),
            .host_address = memory_region_get_ram_ptr(mr),
        };
        g_array_append_val(ranges, range);
    }
    return false;
}

GArray *apple_gfx_new_memory_ranges(void)
{
    GArray *ranges = g_array_new(false, false, sizeof(RvgMemoryRange));
    FlatView *fv = address_space_to_flatview(&address_space_memory);

    flatview_for_each_range(fv, apple_gfx_add_memory_range, ranges);
    return ranges;
}

/*
 * The display's callbacks. The library calls them on its display thread,
 * one at a time, in the order the guest's display events came.
 */
static void apple_gfx_new_frame_cb(void *ctx)
{
    AppleGFXState *s = ctx;
    trace_apple_gfx_new_frame();
    aio_bh_schedule_oneshot(qemu_get_aio_context(), new_frame_handler_bh, s);
}

static void apple_gfx_mode_change_cb(void *ctx, uint16_t width,
                                     uint16_t height, uint32_t pixel_format)
{
    AppleGFXState *s = ctx;
    trace_apple_gfx_mode_change(width, height);

    /*
     * Synchronous, so that the surface is in place before any BH scheduled
     * after it runs.
     */
    BQL_LOCK_GUARD();
    set_mode(s, width, height);
}

static void apple_gfx_cursor_glyph_cb(void *ctx, const RvgCursorGlyph *glyph)
{
    AppleGFXState *s = ctx;
    AppleGFXSetCursorGlyphJob *job = g_new0(AppleGFXSetCursorGlyphJob, 1);
    /* height rows of width * 4 bytes */
    size_t length = (size_t)glyph->width * 4 * glyph->height;

    job->s = s;
    job->width = glyph->width;
    job->height = glyph->height;
    job->hot_spot = glyph->hot_spot;
    job->rgba = g_malloc0(length);
    memcpy(job->rgba, glyph->rgba, MIN(glyph->rgba_length, length));
    aio_bh_schedule_oneshot(qemu_get_aio_context(), set_cursor_glyph, job);
}

/* show is the guest's byte: any value but 0 shows the cursor. */
static void apple_gfx_cursor_show_cb(void *ctx, uint8_t show)
{
    AppleGFXState *s = ctx;
    trace_apple_gfx_cursor_show(show);
    qatomic_set(&s->cursor_show, show != 0);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), update_cursor_bh, s);
}

static void apple_gfx_cursor_move_cb(void *ctx)
{
    AppleGFXState *s = ctx;
    trace_apple_gfx_cursor_move();
    aio_bh_schedule_oneshot(qemu_get_aio_context(), update_cursor_bh, s);
}

static RvgDisplayMode *apple_gfx_new_display_modes(
    const AppleGFXDisplayMode display_modes[], uint32_t display_mode_count)
{
    RvgDisplayMode *modes = g_new0(RvgDisplayMode, display_mode_count);

    for (unsigned i = 0; i < display_mode_count; i++) {
        const AppleGFXDisplayMode *mode = &display_modes[i];
        trace_apple_gfx_display_mode(i, mode->width_px, mode->height_px);

        modes[i] = (RvgDisplayMode) {
            .width = mode->width_px,
            .height = mode->height_px,
            .refresh_rate_hz = mode->refresh_rate_hz,
        };
    }

    return modes;
}

bool apple_gfx_common_realize(AppleGFXState *s, DeviceState *dev,
                              RvgDeviceConfig *config, Error **errp)
{
    static const char display_name[] = "QEMU display";
    g_autoptr(GArray) memory_ranges = NULL;
    g_autofree RvgDisplayMode *modes = NULL;
    const AppleGFXDisplayMode *display_modes = apple_gfx_default_modes;
    uint32_t num_display_modes = ARRAY_SIZE(apple_gfx_default_modes);
    RvgMemoryMap memory_map;
    RvgDisplayCallbacks display_callbacks = {
        .struct_size = sizeof(display_callbacks),
        .ctx = s,
        .mode_change = apple_gfx_mode_change_cb,
        .new_frame = apple_gfx_new_frame_cb,
        .cursor_glyph = apple_gfx_cursor_glyph_cb,
        .cursor_show = apple_gfx_cursor_show_cb,
        .cursor_move = apple_gfx_cursor_move_cb,
    };
    RvgDisplayConfig display_config = {
        .struct_size = sizeof(display_config),
        .port = 0,
        .name = display_name,
        .name_length = sizeof(display_name) - 1,
        .width_millimeters = 400., /* A 20" display */
        .height_millimeters = 300.,
        .callbacks = &display_callbacks,
    };
    RvgStatus status;

    if (apple_gfx_mig_blocker == NULL) {
        error_setg(&apple_gfx_mig_blocker,
                  "Migration state blocked by apple-gfx display device");
        if (migrate_add_blocker(&apple_gfx_mig_blocker, errp) < 0) {
            return false;
        }
    }

    /* The library copies the ranges. */
    memory_ranges = apple_gfx_new_memory_ranges();
    memory_map = (RvgMemoryMap) {
        .ranges = (const RvgMemoryRange *)memory_ranges->data,
        .range_count = memory_ranges->len,
    };
    config->display_port_count = 1;
    config->metal_registry_id = 0; /* the system's default Metal device */
    config->memory_map = &memory_map;

    s->pool = thread_pool_new();
    s->cursor_show = true;

    status = rvg_device_create(config, &s->pgdev);
    if (status != RVG_OK) {
        error_setg(errp, "apple-gfx: rvg_device_create: %s",
                   rvg_status_message(status));
        goto fail;
    }

    /*
     * Although the library does, this integration currently does not support
     * multiple virtual displays connected to a single PV graphics device.
     * It is however possible to create
     * more than one instance of the device, each with one display. The macOS
     * guest will ignore these displays if they share the same serial number,
     * so ensure each instance gets a unique one.
     */
    display_config.serial_number = next_pgdisplay_serial_num++;
    status = rvg_display_create(s->pgdev, &display_config, &s->pgdisp);
    if (status != RVG_OK) {
        error_setg(errp, "apple-gfx: rvg_display_create: %s",
                   rvg_status_message(status));
        goto fail;
    }

    if (s->display_modes != NULL && s->num_display_modes > 0) {
        trace_apple_gfx_common_realize_modes_property(s->num_display_modes);
        display_modes = s->display_modes;
        num_display_modes = s->num_display_modes;
    }
    modes = apple_gfx_new_display_modes(display_modes, num_display_modes);
    rvg_display_set_modes(s->pgdisp, modes, num_display_modes);

    s->con = qemu_graphic_console_create(dev, 0, &apple_gfx_fb_ops, s);
    return true;

fail:
    rvg_device_release(s->pgdev);
    s->pgdev = NULL;
    thread_pool_free(s->pool);
    s->pool = NULL;
    return false;
}

/* ------ Display mode list device property ------ */

static void apple_gfx_get_display_mode(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    Property *prop = opaque;
    AppleGFXDisplayMode *mode = object_field_prop_ptr(obj, prop);
    /* 3 uint16s (max 5 digits) + 2 separator characters + nul. */
    char buffer[5 * 3 + 2 + 1];
    char *pos = buffer;

    int rc = snprintf(buffer, sizeof(buffer),
                      "%"PRIu16"x%"PRIu16"@%"PRIu16,
                      mode->width_px, mode->height_px,
                      mode->refresh_rate_hz);
    assert(rc < sizeof(buffer));

    visit_type_str(v, name, &pos, errp);
}

static void apple_gfx_set_display_mode(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    Property *prop = opaque;
    AppleGFXDisplayMode *mode = object_field_prop_ptr(obj, prop);
    const char *endptr;
    g_autofree char *str = NULL;
    int ret;
    int val;

    if (!visit_type_str(v, name, &str, errp)) {
        return;
    }

    endptr = str;

    ret = qemu_strtoi(endptr, &endptr, 10, &val);
    if (ret || val > UINT16_MAX || val <= 0) {
        error_setg(errp, "width in '%s' must be a decimal integer number"
                         " of pixels in the range 1..65535", name);
        return;
    }
    mode->width_px = val;
    if (*endptr != 'x') {
        goto separator_error;
    }

    ret = qemu_strtoi(endptr + 1, &endptr, 10, &val);
    if (ret || val > UINT16_MAX || val <= 0) {
        error_setg(errp, "height in '%s' must be a decimal integer number"
                         " of pixels in the range 1..65535", name);
        return;
    }
    mode->height_px = val;
    if (*endptr != '@') {
        goto separator_error;
    }

    ret = qemu_strtoi(endptr + 1, &endptr, 10, &val);
    if (ret || val > UINT16_MAX || val <= 0) {
        error_setg(errp, "refresh rate in '%s'"
                         " must be a positive decimal integer (Hertz)", name);
        return;
    }
    mode->refresh_rate_hz = val;
    return;

separator_error:
    error_setg(errp,
               "Each display mode takes the format '<width>x<height>@<rate>'");
}

const PropertyInfo qdev_prop_apple_gfx_display_mode = {
    .type  = "display_mode",
    .description =
        "Display mode in pixels and Hertz, as <width>x<height>@<refresh-rate> "
        "Example: 3840x2160@60",
    .get   = apple_gfx_get_display_mode,
    .set   = apple_gfx_set_display_mode,
};
