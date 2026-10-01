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
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qapi/visitor.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-ui.h"
#include "qemu/aio-wait.h"
#include "system/address-spaces.h"
#include "migration/blocker.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "ui/console.h"
#include "ui/input.h"
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
    AppleGFXDisplay *d;
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
    status = rvg_display_encode_frame(job->d->pgdisp, job->target, NULL,
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

static void apple_gfx_render_new_frame(AppleGFXDisplay *d)
{
    AppleGFXRenderJob *job = g_new0(AppleGFXRenderJob, 1);

    assert(bql_locked());
    job->d = d;
    /* NULL where set_mode's target was refused: the encode then refuses. */
    job->target = d->target ? rvg_frame_target_retain(d->target) : NULL;

    d->rendering_frame_width = surface_width(d->surface);
    d->rendering_frame_height = surface_height(d->surface);

    thread_pool_submit_immediate(d->state->pool, apple_gfx_do_render, job,
                                 NULL);
}

static void apple_gfx_render_frame_completed_bh(void *opaque)
{
    AppleGFXRenderJob *job = opaque;
    AppleGFXDisplay *d = job->d;
    DisplaySurface *surface = d->surface;
    bool window = d->state->window;

    --d->pending_frames;
    assert(d->pending_frames >= 0);

    /* Only update display if mode hasn't changed since we started rendering. */
    if (job->encoded &&
        d->rendering_frame_width == surface_width(surface) &&
        d->rendering_frame_height == surface_height(surface)) {
        /* The target has the surface's size and pixman's x8r8g8b8 layout. */
        rvg_frame_target_read_pixels(job->target, surface_data(surface),
                                     surface_stride(surface),
                                     (size_t)surface_stride(surface) *
                                     surface_height(surface));
        if (window) {
            qemu_console_update_full(d->con);
        } else if (d->gfx_update_requested) {
            d->gfx_update_requested = false;
            qemu_console_update_full(d->con);
            qemu_console_hw_update_done(d->con);
            d->new_frame_ready = false;
        } else {
            d->new_frame_ready = true;
        }
    }
    rvg_frame_target_release(job->target);
    g_free(job);

    if (window) {
        /*
         * The render gfx_update asked for, drawn or not: an update that
         * waits for it, a screendump's, never waits for another.
         */
        qemu_console_hw_update_done(d->con);
    } else if (d->pending_frames > 0) {
        apple_gfx_render_new_frame(d);
    }
}

static void new_frame_handler_bh(void *opaque)
{
    AppleGFXDisplay *d = opaque;

    /* Drop frames if guest gets too far ahead. */
    if (d->pending_frames >= 2) {
        return;
    }
    ++d->pending_frames;
    if (d->pending_frames > 1) {
        return;
    }

    apple_gfx_render_new_frame(d);
}

/*
 * In window mode, the library draws the guest's frames into the display's
 * window, and the surface is drawn here only when QEMU's display system asks
 * for it (a screendump, or a display change listener): one render, whose
 * completion signals the update done.
 */
static bool apple_gfx_window_update_display(AppleGFXDisplay *d)
{
    if (d->pending_frames > 0) {
        /* The render under way signals the update done. */
        return false;
    }
    if (d->surface == NULL || d->target == NULL) {
        /* No mode yet, or its target was refused: the surface stays. */
        return true;
    }

    ++d->pending_frames;
    apple_gfx_render_new_frame(d);
    return false;
}

static bool apple_gfx_fb_update_display(void *opaque)
{
    AppleGFXDisplay *d = opaque;
    bool done = true;

    assert(bql_locked());
    if (d->state->window) {
        return apple_gfx_window_update_display(d);
    }

    if (d->new_frame_ready) {
        qemu_console_update_full(d->con);
        d->new_frame_ready = false;
    } else if (d->pending_frames > 0) {
        d->gfx_update_requested = true;
        done = false;
    }

    return done;
}

static const GraphicHwOps apple_gfx_fb_ops = {
    .gfx_update = apple_gfx_fb_update_display,
};

/* ------ Mouse cursor and display mode setting ------ */

static void set_mode(AppleGFXDisplay *d, uint32_t width, uint32_t height)
{
    RvgFrameTargetConfig target_config = {
        .struct_size = sizeof(target_config),
        .width = width,
        .height = height,
        /* The byte order of the surface's pixman x8r8g8b8. */
        .format = RVG_PIXEL_FORMAT_BGRA8_UNORM,
    };
    RvgStatus status;

    if (d->surface &&
        width == surface_width(d->surface) &&
        height == surface_height(d->surface)) {
        return;
    }

    /* A render under way keeps its own reference. */
    rvg_frame_target_release(d->target);
    d->target = NULL;

    d->surface = qemu_create_displaysurface(width, height);

    status = rvg_frame_target_create(d->state->pgdev, &target_config,
                                     &d->target);
    if (status != RVG_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: rvg_frame_target_create(%" PRIu32 "x%" PRIu32
                      "): %s\n", __func__, width, height,
                      rvg_status_message(status));
    }

    qemu_console_set_surface(d->con, d->surface);
}

static void update_cursor(AppleGFXDisplay *d)
{
    RvgDisplayCoord position;

    assert(bql_locked());
    position = rvg_display_cursor_position(d->pgdisp);
    qemu_console_set_mouse(d->con, position.x, position.y, d->cursor_show);
}

/* A copy of the RvgCursorGlyph, which is borrowed for its callback. */
typedef struct AppleGFXCursorGlyph {
    uint8_t *rgba;
    uint32_t width;
    uint32_t height;
    RvgDisplayCoord hot_spot;
} AppleGFXCursorGlyph;

static void set_cursor_glyph(AppleGFXDisplay *d,
                             const AppleGFXCursorGlyph *glyph)
{
    const uint8_t *px_data = glyph->rgba;
    uint32_t *dest_px;

    /* An RvgCursorGlyph has 32 bits a pixel, and width * 4 bytes a row. */
    trace_apple_gfx_cursor_set(32, glyph->width, glyph->height);

    if (d->cursor) {
        cursor_unref(d->cursor);
        d->cursor = NULL;
    }

    d->cursor = cursor_alloc(glyph->width, glyph->height);
    d->cursor->hot_x = glyph->hot_spot.x;
    d->cursor->hot_y = glyph->hot_spot.y;

    dest_px = d->cursor->data;
    for (uint32_t y = 0; y < glyph->height; ++y) {
        for (uint32_t x = 0; x < glyph->width; ++x) {
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
    qemu_console_set_cursor(d->con, d->cursor);
    update_cursor(d);
}

/* ------ Input from the displays' windows ------ */

/* The most wheel notches one wheel callback sends the guest. */
#define APPLE_GFX_WHEEL_MAX_NOTCHES 256

static void apple_gfx_input_pointer(AppleGFXDisplay *d, uint16_t x,
                                    uint16_t y, uint16_t width,
                                    uint16_t height, uint32_t buttons)
{
    static uint32_t button_map[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT] = RVG_WINDOW_BUTTON_LEFT,
        [INPUT_BUTTON_RIGHT] = RVG_WINDOW_BUTTON_RIGHT,
        [INPUT_BUTTON_MIDDLE] = RVG_WINDOW_BUTTON_MIDDLE,
        [INPUT_BUTTON_SIDE] = RVG_WINDOW_BUTTON_SIDE,
        [INPUT_BUTTON_EXTRA] = RVG_WINDOW_BUTTON_EXTRA,
    };
    AppleGFXState *s = d->state;

    /* QEMU's input layer does not clamp absolute axes. */
    width = MAX(width, 1);
    height = MAX(height, 1);
    qemu_input_queue_abs(d->con, INPUT_AXIS_X, MIN(x, width - 1), 0, width);
    qemu_input_queue_abs(d->con, INPUT_AXIS_Y, MIN(y, height - 1), 0, height);
    qemu_input_update_buttons(d->con, button_map, s->input_buttons, buttons);
    s->input_buttons = buttons;
    qemu_input_event_sync();
}

/*
 * @notches wheel notches on one axis: a press and release of @up for each
 * notch above 0, of @down for each below.
 */
static void apple_gfx_input_wheel_axis(AppleGFXDisplay *d, int32_t notches,
                                       InputButton up, InputButton down)
{
    InputButton button = notches > 0 ? up : down;
    int64_t count = MIN(ABS((int64_t)notches), APPLE_GFX_WHEEL_MAX_NOTCHES);

    for (int64_t i = 0; i < count; i++) {
        qemu_input_queue_btn(d->con, button, true);
        qemu_input_queue_btn(d->con, button, false);
        qemu_input_event_sync();
    }
}

/*
 * A scroll event: smooth scrolling where the device that takes the wheel
 * takes it, the event's notches elsewhere.
 */
static void apple_gfx_input_scroll(AppleGFXDisplay *d,
                                   const RvgWindowScroll *scroll)
{
    static const InputScrollPhase phases[] = {
        [RVG_WINDOW_SCROLL_PHASE_NONE] = INPUT_SCROLL_PHASE_NONE,
        [RVG_WINDOW_SCROLL_PHASE_MAY_BEGIN] = INPUT_SCROLL_PHASE_MAY_BEGIN,
        [RVG_WINDOW_SCROLL_PHASE_BEGAN] = INPUT_SCROLL_PHASE_BEGAN,
        [RVG_WINDOW_SCROLL_PHASE_CHANGED] = INPUT_SCROLL_PHASE_CHANGED,
        [RVG_WINDOW_SCROLL_PHASE_ENDED] = INPUT_SCROLL_PHASE_ENDED,
        [RVG_WINDOW_SCROLL_PHASE_CANCELLED] = INPUT_SCROLL_PHASE_CANCELLED,
    };
    static const InputScrollMomentum momenta[] = {
        [RVG_WINDOW_SCROLL_MOMENTUM_NONE] = INPUT_SCROLL_MOMENTUM_NONE,
        [RVG_WINDOW_SCROLL_MOMENTUM_BEGAN] = INPUT_SCROLL_MOMENTUM_BEGAN,
        [RVG_WINDOW_SCROLL_MOMENTUM_CHANGED] = INPUT_SCROLL_MOMENTUM_CHANGED,
        [RVG_WINDOW_SCROLL_MOMENTUM_ENDED] = INPUT_SCROLL_MOMENTUM_ENDED,
    };
    InputScrollPhase phase = INPUT_SCROLL_PHASE_NONE;
    InputScrollMomentum momentum = INPUT_SCROLL_MOMENTUM_NONE;

    if (!qemu_input_scrolls_smoothly(d->con)) {
        apple_gfx_input_wheel_axis(d, scroll->notches_dy,
                                   INPUT_BUTTON_WHEEL_UP,
                                   INPUT_BUTTON_WHEEL_DOWN);
        apple_gfx_input_wheel_axis(d, scroll->notches_dx,
                                   INPUT_BUTTON_WHEEL_LEFT,
                                   INPUT_BUTTON_WHEEL_RIGHT);
        return;
    }
    if (scroll->phase < ARRAY_SIZE(phases)) {
        phase = phases[scroll->phase];
    }
    if (scroll->momentum < ARRAY_SIZE(momenta)) {
        momentum = momenta[scroll->momentum];
    }
    /*
     * 1/256 of the host's point, as 1/256 pixel, which a macOS guest scrolls
     * as its point.
     */
    qemu_input_queue_scroll(d->con, INPUT_AXIS_Y, scroll->dy, phase,
                            momentum);
    qemu_input_queue_scroll(d->con, INPUT_AXIS_X, scroll->dx, phase,
                            momentum);
    qemu_input_event_sync();
}

static void apple_gfx_input_key(AppleGFXDisplay *d, uint16_t usage,
                                bool down)
{
    unsigned int lnx = 0;

    if (usage < qemu_input_map_usb_to_linux_len) {
        lnx = qemu_input_map_usb_to_linux[usage];
    }
    if (lnx == 0) {
        return;
    }
    /*
     * At once, as QEMU's cocoa, GTK and SDL displays send keys: no delay
     * between them, which a person's keys, far apart, do not need to fit the
     * USB keyboard's queue of 16 scancodes.
     */
    qemu_input_event_send_key_linux(d->con, lnx, down);
}

static void apple_gfx_input_close(void)
{
    /* As QEMU's own windows do: closing one powers the VM off. */
    shutdown_action = SHUTDOWN_ACTION_POWEROFF;
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}

/* ------ Callbacks' work under the BQL ------ */

typedef enum AppleGFXEventKind {
    APPLE_GFX_EVENT_CURSOR_GLYPH,
    APPLE_GFX_EVENT_CURSOR_SHOW,
    APPLE_GFX_EVENT_CURSOR_MOVE,
    APPLE_GFX_EVENT_POINTER,
    APPLE_GFX_EVENT_WHEEL,
    APPLE_GFX_EVENT_SCROLL,
    APPLE_GFX_EVENT_KEY,
    APPLE_GFX_EVENT_CLOSE,
} AppleGFXEventKind;

struct AppleGFXEvent {
    QSIMPLEQ_ENTRY(AppleGFXEvent) next;
    AppleGFXDisplay *d;
    AppleGFXEventKind kind;
    union {
        AppleGFXCursorGlyph glyph;
        bool show;
        struct {
            uint16_t x;
            uint16_t y;
            uint16_t width;
            uint16_t height;
            uint32_t buttons;
        } pointer;
        struct {
            int32_t dy;
            int32_t dx;
        } wheel;
        RvgWindowScroll scroll;
        struct {
            uint16_t usage;
            bool down;
        } key;
    };
};

static AppleGFXEvent *apple_gfx_event_new(AppleGFXDisplay *d,
                                          AppleGFXEventKind kind)
{
    AppleGFXEvent *event = g_new0(AppleGFXEvent, 1);

    event->d = d;
    event->kind = kind;
    return event;
}

/*
 * Queues @event behind every earlier one, on any thread. One-shot BHs
 * scheduled together run in the reverse order, so each device has one BH,
 * which runs its events in the order they were queued.
 */
static void apple_gfx_event_push(AppleGFXEvent *event)
{
    AppleGFXState *s = event->d->state;

    WITH_QEMU_LOCK_GUARD(&s->event_lock) {
        QSIMPLEQ_INSERT_TAIL(&s->events, event, next);
    }
    qemu_bh_schedule(s->event_bh);
}

static void apple_gfx_event_run(AppleGFXEvent *event)
{
    AppleGFXDisplay *d = event->d;

    switch (event->kind) {
    case APPLE_GFX_EVENT_CURSOR_GLYPH:
        set_cursor_glyph(d, &event->glyph);
        g_free(event->glyph.rgba);
        break;
    case APPLE_GFX_EVENT_CURSOR_SHOW:
        d->cursor_show = event->show;
        update_cursor(d);
        break;
    case APPLE_GFX_EVENT_CURSOR_MOVE:
        update_cursor(d);
        break;
    case APPLE_GFX_EVENT_POINTER:
        apple_gfx_input_pointer(d, event->pointer.x, event->pointer.y,
                                event->pointer.width, event->pointer.height,
                                event->pointer.buttons);
        break;
    case APPLE_GFX_EVENT_WHEEL:
        apple_gfx_input_wheel_axis(d, event->wheel.dy, INPUT_BUTTON_WHEEL_UP,
                                   INPUT_BUTTON_WHEEL_DOWN);
        apple_gfx_input_wheel_axis(d, event->wheel.dx,
                                   INPUT_BUTTON_WHEEL_LEFT,
                                   INPUT_BUTTON_WHEEL_RIGHT);
        break;
    case APPLE_GFX_EVENT_SCROLL:
        apple_gfx_input_scroll(d, &event->scroll);
        break;
    case APPLE_GFX_EVENT_KEY:
        apple_gfx_input_key(d, event->key.usage, event->key.down);
        break;
    case APPLE_GFX_EVENT_CLOSE:
        apple_gfx_input_close();
        break;
    default:
        g_assert_not_reached();
    }
}

static void apple_gfx_event_bh(void *opaque)
{
    AppleGFXState *s = opaque;
    AppleGFXEvent *event;

    assert(bql_locked());
    /* One at a time, so a nested run still takes them in order. */
    for (;;) {
        WITH_QEMU_LOCK_GUARD(&s->event_lock) {
            event = QSIMPLEQ_FIRST(&s->events);
            if (event) {
                QSIMPLEQ_REMOVE_HEAD(&s->events, next);
            }
        }
        if (event == NULL) {
            break;
        }
        apple_gfx_event_run(event);
        g_free(event);
    }
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
    AppleGFXDisplay *d = ctx;
    trace_apple_gfx_new_frame();
    if (d->state->window) {
        /* The window draws it; gfx_update renders the surface on demand. */
        return;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), new_frame_handler_bh, d);
}

static void apple_gfx_mode_change_cb(void *ctx, uint16_t width,
                                     uint16_t height, uint32_t pixel_format)
{
    AppleGFXDisplay *d = ctx;
    trace_apple_gfx_mode_change(width, height);

    /*
     * Synchronous, so that the surface is in place before any BH scheduled
     * after it runs.
     */
    BQL_LOCK_GUARD();
    set_mode(d, width, height);
}

static void apple_gfx_cursor_glyph_cb(void *ctx, const RvgCursorGlyph *glyph)
{
    AppleGFXDisplay *d = ctx;
    AppleGFXEvent *event = apple_gfx_event_new(d,
                                               APPLE_GFX_EVENT_CURSOR_GLYPH);
    /* height rows of width * 4 bytes */
    size_t length = (size_t)glyph->width * 4 * glyph->height;

    event->glyph.width = glyph->width;
    event->glyph.height = glyph->height;
    event->glyph.hot_spot = glyph->hot_spot;
    event->glyph.rgba = g_malloc0(length);
    memcpy(event->glyph.rgba, glyph->rgba, MIN(glyph->rgba_length, length));
    apple_gfx_event_push(event);
}

/* show is the guest's byte: any value but 0 shows the cursor. */
static void apple_gfx_cursor_show_cb(void *ctx, uint8_t show)
{
    AppleGFXDisplay *d = ctx;
    AppleGFXEvent *event = apple_gfx_event_new(d, APPLE_GFX_EVENT_CURSOR_SHOW);

    trace_apple_gfx_cursor_show(show);
    event->show = show != 0;
    apple_gfx_event_push(event);
}

static void apple_gfx_cursor_move_cb(void *ctx)
{
    AppleGFXDisplay *d = ctx;
    trace_apple_gfx_cursor_move();
    apple_gfx_event_push(apple_gfx_event_new(d, APPLE_GFX_EVENT_CURSOR_MOVE));
}

/*
 * The window's callbacks. The library calls them on the process's main
 * thread, which QEMU leaves in its run loop under -display none, without the
 * BQL: they queue the input for the BQL, and never block.
 */
static void apple_gfx_window_pointer_cb(void *ctx, uint16_t x, uint16_t y,
                                        uint16_t width, uint16_t height,
                                        uint32_t buttons)
{
    AppleGFXEvent *event = apple_gfx_event_new(ctx, APPLE_GFX_EVENT_POINTER);

    event->pointer.x = x;
    event->pointer.y = y;
    event->pointer.width = width;
    event->pointer.height = height;
    event->pointer.buttons = buttons;
    apple_gfx_event_push(event);
}

/* dy above 0 is up, dx above 0 is left. */
static void apple_gfx_window_wheel_cb(void *ctx, int32_t dy, int32_t dx)
{
    AppleGFXEvent *event = apple_gfx_event_new(ctx, APPLE_GFX_EVENT_WHEEL);

    event->wheel.dy = dy;
    event->wheel.dx = dx;
    apple_gfx_event_push(event);
}

static void apple_gfx_window_scroll_cb(void *ctx,
                                       const RvgWindowScroll *scroll)
{
    AppleGFXEvent *event = apple_gfx_event_new(ctx, APPLE_GFX_EVENT_SCROLL);

    /* The fields the library's version has; the rest stay 0. */
    memcpy(&event->scroll, scroll,
           MIN(scroll->struct_size, sizeof(event->scroll)));
    apple_gfx_event_push(event);
}

/* usage is a USB HID keyboard usage (page 0x07). */
static void apple_gfx_window_key_cb(void *ctx, uint16_t usage, bool down)
{
    AppleGFXEvent *event = apple_gfx_event_new(ctx, APPLE_GFX_EVENT_KEY);

    event->key.usage = usage;
    event->key.down = down;
    apple_gfx_event_push(event);
}

static void apple_gfx_window_close_cb(void *ctx)
{
    apple_gfx_event_push(apple_gfx_event_new(ctx, APPLE_GFX_EVENT_CLOSE));
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

/*
 * Window mode: under -display none, QEMU shows no display of its own, and the
 * library opens a window for each, unless REIMS_VGPU_WINDOW is "0".
 */
static bool apple_gfx_window_wanted(void)
{
    static bool reported;
    DisplayOptions *options = qmp_query_display_options(&error_abort);
    bool window = options->type == DISPLAY_TYPE_NONE &&
                  g_strcmp0(g_getenv("REIMS_VGPU_WINDOW"), "0") != 0;

    qapi_free_DisplayOptions(options);
    if (window && !reported) {
        reported = true;
        info_report("apple-gfx: -display none: each display opens a window "
                    "of its own; REIMS_VGPU_WINDOW=0 keeps QEMU headless");
    }
    return window;
}

static void apple_gfx_events_free(AppleGFXState *s)
{
    AppleGFXEvent *event;

    while ((event = QSIMPLEQ_FIRST(&s->events)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&s->events, next);
        if (event->kind == APPLE_GFX_EVENT_CURSOR_GLYPH) {
            g_free(event->glyph.rgba);
        }
        g_free(event);
    }
}

/*
 * The whole centimetres a macOS guest reads a mode @width pixels wide as,
 * told @ppi pixels per inch: the library tells it width / (ppi / 25.46)
 * millimetres, which the display's settings page holds as a float and in
 * whole millimetres, rounded to nearest, and the guest rounds those up to
 * whole centimetres.
 */
static uint64_t apple_gfx_guest_centimeters(int width, int ppi)
{
    float millimeters = width / (ppi / 25.46);
    uint64_t whole = (uint64_t)((double)millimeters + 0.5);

    return (whole + 9) / 10;
}

/*
 * Whether the scale a macOS guest runs a mode @width pixels wide at, told
 * @ppi pixels per inch, is not known from the density. Measured on macOS
 * 13.6, the guest runs HiDPI from 2021 pixels across 35 whole centimetres
 * (146.667 ppi), for modes from 1920 pixels wide and 29 cm, and 1x at up to
 * 3811 pixels across 66 (146.666 ppi), for modes up to 3811 pixels wide.
 * Modes of other widths need not follow it: a mode 5832 pixels wide ran
 * HiDPI at 128.8 ppi, and 1280x800 ran 1x at 216.7 ppi across 15 cm.
 */
static bool apple_gfx_guest_scale_unknown(int width, int ppi)
{
    uint64_t cm = apple_gfx_guest_centimeters(width, ppi);
    uint64_t w = width;

    if (cm == 0) {
        return true;
    }
    if (w * 35 >= 2021 * cm) {
        return width < 1920 || cm < 29;
    }
    return width > 3811 || w * 66 > 3811 * cm;
}

/*
 * The mode a window display offers the guest first, from the resolution
 * property: NULL is 1920x1080, "host" is none (0x0: the host screen's own
 * modes, at its density), else "<width>x<height>", each 1 to 65535 pixels,
 * optionally followed by "@<ppi>", the density the guest is told for the
 * mode, 1 to 65535 pixels per inch, refused where the scale the guest runs
 * the mode at is not known from it. Without it *ppi is 0, which the library
 * takes as its 1x density. Only this mode is checked: the smaller modes
 * offered with it are told the same size.
 */
static bool apple_gfx_parse_resolution(const char *str, uint16_t *width,
                                       uint16_t *height, double *ppi,
                                       Error **errp)
{
    const char *endptr;
    int w, h, p = 0;

    *ppi = 0;
    if (str == NULL) {
        *width = 1920;
        *height = 1080;
        return true;
    }
    if (strcmp(str, "host") == 0) {
        *width = 0;
        *height = 0;
        return true;
    }
    if (qemu_strtoi(str, &endptr, 10, &w) || w <= 0 || w > UINT16_MAX ||
        *endptr != 'x' ||
        qemu_strtoi(endptr + 1, &endptr, 10, &h) || h <= 0 ||
        h > UINT16_MAX ||
        (*endptr == '@' &&
         (qemu_strtoi(endptr + 1, NULL, 10, &p) || p <= 0 ||
          p > UINT16_MAX)) ||
        (*endptr != '@' && *endptr != '\0')) {
        error_setg(errp, "apple-gfx: resolution must be <width>x<height>, "
                   "each 1 to 65535 pixels, optionally followed by @<ppi>, "
                   "1 to 65535, or host, not '%s'", str);
        return false;
    }
    if (p != 0 && apple_gfx_guest_scale_unknown(w, p)) {
        error_setg(errp, "apple-gfx: resolution '%s' tells a macOS guest "
                   "%d pixels across %" PRIu64 " cm, where the scale it runs "
                   "is not known from the density; choose another density",
                   str, w, apple_gfx_guest_centimeters(w, p));
        return false;
    }
    *width = w;
    *height = h;
    *ppi = p;
    return true;
}

bool apple_gfx_common_realize(AppleGFXState *s, DeviceState *dev,
                              RvgDeviceConfig *config, Error **errp)
{
    static const char display_name[] = "QEMU display";
    g_autoptr(GArray) memory_ranges = NULL;
    g_autofree RvgDisplayMode *modes = NULL;
    const AppleGFXDisplayMode *display_modes = apple_gfx_default_modes;
    uint32_t num_display_modes = ARRAY_SIZE(apple_gfx_default_modes);
    bool modes_property = s->display_modes != NULL &&
                          s->num_display_modes > 0;
    RvgMemoryMap memory_map;
    RvgDisplayCallbacks display_callbacks = {
        .struct_size = sizeof(display_callbacks),
        .mode_change = apple_gfx_mode_change_cb,
        .new_frame = apple_gfx_new_frame_cb,
        .cursor_glyph = apple_gfx_cursor_glyph_cb,
        .cursor_show = apple_gfx_cursor_show_cb,
        .cursor_move = apple_gfx_cursor_move_cb,
    };
    RvgWindowCallbacks window_callbacks = {
        .struct_size = sizeof(window_callbacks),
        .pointer = apple_gfx_window_pointer_cb,
        .wheel = apple_gfx_window_wheel_cb,
        .key = apple_gfx_window_key_cb,
        .close = apple_gfx_window_close_cb,
        .scroll = apple_gfx_window_scroll_cb,
    };
    /*
     * Unless display-modes names some, the library offers the resolution
     * property's mode at 1x or at the density it names, or the host screen's
     * own modes, at the host screen's refresh rate.
     */
    RvgWindowConfig window_config = {
        .struct_size = sizeof(window_config),
        .flags = modes_property ? 0 : RVG_WINDOW_MATCH_HOST_SCREEN,
        .callbacks = &window_callbacks,
    };
    RvgDisplayConfig display_config = {
        .struct_size = sizeof(display_config),
        .name = display_name,
        .name_length = sizeof(display_name) - 1,
        .width_millimeters = 400., /* A 20" display */
        .height_millimeters = 300.,
        .callbacks = &display_callbacks,
    };
    RvgStatus status;
    uint32_t i;

    if (s->num_displays < 1 || s->num_displays > RVG_MAX_DISPLAY_PORTS) {
        error_setg(errp, "apple-gfx: displays must be 1 to %u, not %" PRIu32,
                   RVG_MAX_DISPLAY_PORTS, s->num_displays);
        return false;
    }
    if (!apple_gfx_parse_resolution(s->resolution, &window_config.mode_width,
                                    &window_config.mode_height,
                                    &window_config.mode_pixels_per_inch,
                                    errp)) {
        return false;
    }

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
    config->display_port_count = s->num_displays;
    config->metal_registry_id = 0; /* the system's default Metal device */
    config->memory_map = &memory_map;

    s->pool = thread_pool_new();
    qemu_mutex_init(&s->event_lock);
    QSIMPLEQ_INIT(&s->events);
    s->event_bh = qemu_bh_new(apple_gfx_event_bh, s);
    s->window = apple_gfx_window_wanted();
    for (i = 0; i < s->num_displays; i++) {
        s->displays[i].state = s;
        s->displays[i].cursor_show = true;
    }

    status = rvg_device_create(config, &s->pgdev);
    if (status != RVG_OK) {
        error_setg(errp, "apple-gfx: rvg_device_create: %s",
                   rvg_status_message(status));
        goto fail;
    }

    if (modes_property) {
        trace_apple_gfx_common_realize_modes_property(s->num_display_modes);
        display_modes = s->display_modes;
        num_display_modes = s->num_display_modes;
    }
    if (!s->window || modes_property) {
        modes = apple_gfx_new_display_modes(display_modes, num_display_modes);
    }

    for (i = 0; i < s->num_displays; i++) {
        AppleGFXDisplay *d = &s->displays[i];

        display_callbacks.ctx = d;
        window_callbacks.ctx = d;
        display_config.port = i;
        display_config.window = s->window ? &window_config : NULL;
        /*
         * The macOS guest ignores displays that share a serial number, those
         * of other instances of the device among them, so each display gets
         * a unique one.
         */
        display_config.serial_number = next_pgdisplay_serial_num++;
        status = rvg_display_create(s->pgdev, &display_config, &d->pgdisp);
        if (status != RVG_OK) {
            error_setg(errp, "apple-gfx: rvg_display_create: %s",
                       rvg_status_message(status));
            goto fail;
        }

        /* A window matching the host screen offers the guest its modes. */
        if (modes) {
            rvg_display_set_modes(d->pgdisp, modes, num_display_modes);
        }
    }

    for (i = 0; i < s->num_displays; i++) {
        s->displays[i].con = qemu_graphic_console_create(dev, i,
                                                         &apple_gfx_fb_ops,
                                                         &s->displays[i]);
    }
    return true;

fail:
    for (i = s->num_displays; i-- > 0;) {
        rvg_display_release(s->displays[i].pgdisp);
        s->displays[i].pgdisp = NULL;
    }
    rvg_device_release(s->pgdev);
    s->pgdev = NULL;
    thread_pool_free(s->pool);
    s->pool = NULL;
    qemu_bh_delete(s->event_bh);
    s->event_bh = NULL;
    apple_gfx_events_free(s);
    qemu_mutex_destroy(&s->event_lock);
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
