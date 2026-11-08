/*
 * QEMU reims-graphics device, PCI 1234:5247
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The GPU device of reims-vgpu's own guest driver, for x86-64 macOS guests:
 * a device of its own ID, which Apple's paravirtualized graphics kext never
 * matches. Its host side is reims-vgpu's graphics device
 * (rvg_graphics_device_*, reims_vgpu.h); this file is its transport:
 *
 *  - BAR 0: the library's 4 KiB of 32-bit registers, with the MSI-X table
 *    and PBA from RVG_GRAPHICS_MSIX_OFFSET, and one vector. A guest write of
 *    RVG_GRAPHICS_DOORBELL_CONTROL to the DOORBELL register is taken by an
 *    ioeventfd, which the library's control thread polls, so ringing the
 *    control ring never runs the library on the vCPU. A write of a command
 *    queue's ring goes to the library on the vCPU, which may answer the
 *    queue's next command buffer there, posting the event that says it is
 *    submitted, and leave the buffer to the queue's thread
 *    (rvg_graphics_device_mmio_write_taking).
 *  - BAR 2: the library's region, 64-bit and prefetchable, memory of QEMU's
 *    process (on Linux a shared mapping of a memfd of the library's, on macOS
 *    private anonymous memory) that the guest maps write-back: the control
 *    rings below RVG_GRAPHICS_ALLOCATIONS_OFFSET, the guest's allocations
 *    from there. It is one RAM region over the mapping, with no dirty log:
 *    the library's window shows the guest's scanout again at each of the
 *    display's refreshes while it shows no presented surface.
 *  - The library's callbacks run on its own threads, and raise_interrupt
 *    also on the vCPU inside that write, with no lock held that a call of
 *    ours into the library waits for. An interrupt is raised by a write of
 *    the vector's irqfd, where the accelerator injects MSI-X through one
 *    (KVM's in-kernel irqchip) and MSI-X is enabled, and by a BH otherwise;
 *    neither waits for a lock.
 *
 * Under -display none the library opens a window for the display, unless
 * REIMS_VGPU_WINDOW is "0": it opens at the resolution
 * property's size, "<width>x<height>" (1920x1080 without it), then keeps the
 * size the person gives it, and the guest follows it. Its input reaches the
 * guest through the device's console. The console shows what the display shows,
 * read back from the library on a thread of the device's pool at each of its
 * updates (rvg_graphics_device_read_display): QEMU's own displays and its
 * screendump read it there. The console also has the guest's cursor, which the
 * library tells the device on its display thread and a BH gives the console, so
 * QEMU's own displays (SPICE, VNC, GTK, D-Bus) draw it; the image read back has
 * none, and under -display none the console has no display, where the window
 * shows the cursor as the host's pointer.
 *
 * The device has no default option ROM: romfile names one.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/event_notifier.h"
#include "qemu/lockable.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/queue.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-ui.h"
#include "migration/blocker.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "system/accel-irq.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/ramblock.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "block/thread-pool.h"
#include "ui/console.h"
#include "ui/input.h"
#include <reims_vgpu.h>

#define TYPE_REIMS_GRAPHICS_PCI "reims-graphics-pci"
OBJECT_DECLARE_SIMPLE_TYPE(ReimsGraphicsPCIState, REIMS_GRAPHICS_PCI)

/* The PBA, after the table's one 16-byte entry, aligned as MSI-X needs. */
#define REIMS_GRAPHICS_PCI_PBA_OFFSET (RVG_GRAPHICS_MSIX_OFFSET + 0x400)

/* The most wheel notches one scroll event sends the guest, on each axis. */
#define REIMS_GRAPHICS_PCI_WHEEL_MAX_NOTCHES 256

typedef struct ReimsGraphicsEvent ReimsGraphicsEvent;

struct ReimsGraphicsPCIState {
    PCIDevice parent_obj;

    /* Initialised on init/realize() */
    MemoryRegion bar0;      /* BAR 0: the registers, the MSI-X table, PBA */
    MemoryRegion registers; /* in BAR 0, below the MSI-X table */
    MemoryRegion region;    /* BAR 2 */
    EventNotifier doorbell;
    bool doorbell_made;  /* the notifier is initialised */
    bool doorbell_added; /* the notifier is BAR 0's ioeventfd */
    EventNotifier irqfd; /* the vector's, where the accelerator has irqfds */
    bool irqfd_made;     /* the notifier is initialised */
    RvgGraphicsDevice *device;
    QemuConsole *con;
    ThreadPool *pool; /* where the console's reads of the display run */

    /* The window's input, queued on its thread and run under the BQL. */
    QemuMutex event_lock;
    QSIMPLEQ_HEAD(, ReimsGraphicsEvent) events; /* event_lock protected */
    QEMUBH *event_bh;

    char *resolution; /* the property: the window's first size, or NULL */

    /* Mutable state (BQL protected) */
    uint32_t input_buttons; /* RVG_WINDOW_BUTTON_* bits the guest was sent */
    QEMUCursor *cursor;     /* the guest's cursor image, or NULL */
    bool cursor_show;       /* the library says the cursor shows */
    bool reading;           /* a read of the display is under way */
    DisplaySurface *surface; /* the console's, which the reads fill */
    uint8_t *pixels;        /* the last read's buffer, kept for the next */
    size_t pixels_length;
    int irqfd_virq;         /* the vector's MSI route while MSI-X is enabled */
    bool irqfd_unmasked;    /* the irqfd is the route's */
    /* Atomic, set under the BQL: whether a raise writes the irqfd. */
    bool irqfd_on;
};

QEMU_BUILD_BUG_ON(RVG_GRAPHICS_MSIX_VECTORS != 1);

static Error *reims_graphics_mig_blocker;

/* ------ Interrupts ------ */

typedef struct ReimsGraphicsInterruptJob {
    PCIDevice *device;
    uint32_t vector;
} ReimsGraphicsInterruptJob;

static void reims_graphics_pci_raise_interrupt(void *opaque)
{
    ReimsGraphicsInterruptJob *job = opaque;

    if (msix_enabled(job->device)) {
        msix_notify(job->device, job->vector);
    }
    g_free(job);
}

/*
 * On the library's threads, or on the vCPU inside a register write
 * (rvg_graphics_device_mmio_write_taking, which holds no lock of ours), after
 * the library's write of what the interrupt reports: the irqfd's write
 * follows it on the same thread, so the guest takes the interrupt after it
 * sees the write, as it does through the BH. Neither waits for a lock.
 */
static void reims_graphics_pci_raise_interrupt_cb(void *ctx, uint32_t vector)
{
    ReimsGraphicsPCIState *s = ctx;
    ReimsGraphicsInterruptJob *job;

    if (qatomic_load_acquire(&s->irqfd_on)) {
        event_notifier_set(&s->irqfd);
        return;
    }
    job = g_new0(ReimsGraphicsInterruptJob, 1);
    job->device = ctx;
    job->vector = vector;
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            reims_graphics_pci_raise_interrupt, job);
}

/*
 * The vector through its irqfd, set up while MSI-X is enabled, as ivshmem
 * does. What the guest sees is what msix_notify gives it: an interrupt the
 * vector takes while it is masked is its pending bit, and one raised while
 * MSI-X is disabled is dropped.
 */

static int reims_graphics_pci_vector_unmask(PCIDevice *dev, unsigned vector,
                                            MSIMessage msg)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(dev);
    int ret;

    ret = accel_irqchip_update_msi_route(s->irqfd_virq, msg, dev);
    if (ret < 0) {
        return ret;
    }
    accel_irqchip_commit_routes();
    /*
     * Raised while the vector was masked: its pending bit, as msix_notify sets
     * it, which msix_handle_mask_update delivers once this returns.
     */
    if (event_notifier_test_and_clear(&s->irqfd)) {
        msix_set_pending(dev, vector);
    }
    ret = accel_irqchip_add_irqfd_notifier_gsi(&s->irqfd, NULL, s->irqfd_virq);
    if (ret < 0) {
        return ret;
    }
    s->irqfd_unmasked = true;
    return 0;
}

static void reims_graphics_pci_vector_mask(PCIDevice *dev, unsigned vector)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(dev);

    if (accel_irqchip_remove_irqfd_notifier_gsi(&s->irqfd,
                                                s->irqfd_virq) < 0) {
        error_report("%s: remove_irqfd_notifier_gsi failed",
                     TYPE_REIMS_GRAPHICS_PCI);
        return;
    }
    s->irqfd_unmasked = false;
}

static void reims_graphics_pci_vector_poll(PCIDevice *dev,
                                           unsigned int vector_start,
                                           unsigned int vector_end)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(dev);

    if (vector_start == 0 && vector_end > 0 && msix_is_masked(dev, 0) &&
        event_notifier_test_and_clear(&s->irqfd)) {
        msix_set_pending(dev, 0);
    }
}

static void reims_graphics_pci_irqfd_enable(ReimsGraphicsPCIState *s)
{
    PCIDevice *dev = PCI_DEVICE(s);
    AccelRouteChange c;
    int virq;

    /* Raised while MSI-X was disabled: dropped, as the BH drops it. */
    event_notifier_test_and_clear(&s->irqfd);
    c = accel_irqchip_begin_route_changes();
    virq = accel_irqchip_add_msi_route(&c, 0, dev);
    if (virq < 0) {
        error_report("%s: add_msi_route failed: interrupts go through a BH",
                     TYPE_REIMS_GRAPHICS_PCI);
        return;
    }
    accel_irqchip_commit_route_changes(&c);
    s->irqfd_virq = virq;
    if (msix_set_vector_notifiers(dev, reims_graphics_pci_vector_unmask,
                                  reims_graphics_pci_vector_mask,
                                  reims_graphics_pci_vector_poll) < 0) {
        error_report("%s: set_vector_notifiers failed: interrupts go "
                     "through a BH", TYPE_REIMS_GRAPHICS_PCI);
        accel_irqchip_release_virq(virq);
        s->irqfd_virq = -1;
        return;
    }
    qatomic_store_release(&s->irqfd_on, true);
}

/*
 * A raise that still writes the irqfd after this leaves a count there, which
 * the next enable drops.
 */
static void reims_graphics_pci_irqfd_disable(ReimsGraphicsPCIState *s)
{
    PCIDevice *dev = PCI_DEVICE(s);

    if (!dev->msix_vector_use_notifier) {
        return;
    }
    qatomic_store_release(&s->irqfd_on, false);
    msix_unset_vector_notifiers(dev);
    /* Where MSI-X was already disabled, unset called no mask notifier. */
    if (s->irqfd_unmasked) {
        reims_graphics_pci_vector_mask(dev, 0);
    }
    accel_irqchip_release_virq(s->irqfd_virq);
    s->irqfd_virq = -1;
}

static void reims_graphics_pci_write_config(PCIDevice *dev, uint32_t address,
                                            uint32_t val, int len)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(dev);
    bool was_enabled = msix_enabled(dev);

    pci_default_write_config(dev, address, val, len);
    if (!s->irqfd_made) {
        return;
    }
    if (!was_enabled && msix_enabled(dev)) {
        reims_graphics_pci_irqfd_enable(s);
    } else if (was_enabled && !msix_enabled(dev)) {
        reims_graphics_pci_irqfd_disable(s);
    }
}

/* ------ Input from the display's window ------ */

typedef enum ReimsGraphicsEventKind {
    REIMS_GRAPHICS_EVENT_POINTER,
    REIMS_GRAPHICS_EVENT_SCROLL,
    REIMS_GRAPHICS_EVENT_KEY,
    REIMS_GRAPHICS_EVENT_CLOSE,
    REIMS_GRAPHICS_EVENT_CURSOR_GLYPH,
    REIMS_GRAPHICS_EVENT_CURSOR_SHOW,
    REIMS_GRAPHICS_EVENT_CURSOR_MOVE,
} ReimsGraphicsEventKind;

struct ReimsGraphicsEvent {
    QSIMPLEQ_ENTRY(ReimsGraphicsEvent) next;
    ReimsGraphicsEventKind kind;
    union {
        struct {
            uint16_t x;
            uint16_t y;
            uint16_t width;
            uint16_t height;
            uint32_t buttons;
        } pointer;
        RvgWindowScroll scroll;
        struct {
            uint16_t usage;
            bool down;
        } key;
        /* A copy of the RvgCursorGlyph, which is borrowed for its callback. */
        struct {
            uint8_t *rgba; /* height rows of width * 4 bytes, RGBA */
            uint32_t width;
            uint32_t height;
            RvgDisplayCoord hot_spot;
        } glyph;
        bool show;
    };
};

static void reims_graphics_event_free(ReimsGraphicsEvent *event)
{
    if (event->kind == REIMS_GRAPHICS_EVENT_CURSOR_GLYPH) {
        g_free(event->glyph.rgba);
    }
    g_free(event);
}

static void reims_graphics_input_pointer(ReimsGraphicsPCIState *s,
                                         ReimsGraphicsEvent *event)
{
    static uint32_t button_map[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT] = RVG_WINDOW_BUTTON_LEFT,
        [INPUT_BUTTON_RIGHT] = RVG_WINDOW_BUTTON_RIGHT,
        [INPUT_BUTTON_MIDDLE] = RVG_WINDOW_BUTTON_MIDDLE,
        [INPUT_BUTTON_SIDE] = RVG_WINDOW_BUTTON_SIDE,
        [INPUT_BUTTON_EXTRA] = RVG_WINDOW_BUTTON_EXTRA,
    };
    uint16_t width = MAX(event->pointer.width, 1);
    uint16_t height = MAX(event->pointer.height, 1);

    /* QEMU's input layer does not clamp absolute axes. */
    qemu_input_queue_abs(s->con, INPUT_AXIS_X,
                         MIN(event->pointer.x, width - 1), 0, width);
    qemu_input_queue_abs(s->con, INPUT_AXIS_Y,
                         MIN(event->pointer.y, height - 1), 0, height);
    qemu_input_update_buttons(s->con, button_map, s->input_buttons,
                              event->pointer.buttons);
    s->input_buttons = event->pointer.buttons;
    qemu_input_event_sync();
}

/*
 * @notches wheel notches on one axis: a press and release of @up for each
 * notch above 0, of @down for each below.
 */
static void reims_graphics_input_wheel_axis(ReimsGraphicsPCIState *s,
                                            int32_t notches, InputButton up,
                                            InputButton down)
{
    InputButton button = notches > 0 ? up : down;
    int64_t count = MIN(ABS((int64_t)notches),
                        REIMS_GRAPHICS_PCI_WHEEL_MAX_NOTCHES);

    for (int64_t i = 0; i < count; i++) {
        qemu_input_queue_btn(s->con, button, true);
        qemu_input_queue_btn(s->con, button, false);
        qemu_input_event_sync();
    }
}

/*
 * A scroll event: smooth scrolling where the device that takes the wheel
 * takes it, the event's notches elsewhere, up and left above 0.
 */
static void reims_graphics_input_scroll(ReimsGraphicsPCIState *s,
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

    if (!qemu_input_scrolls_smoothly(s->con)) {
        reims_graphics_input_wheel_axis(s, scroll->notches_dy,
                                        INPUT_BUTTON_WHEEL_UP,
                                        INPUT_BUTTON_WHEEL_DOWN);
        reims_graphics_input_wheel_axis(s, scroll->notches_dx,
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
    /* 1/256 of the host's point, as 1/256 pixel. */
    qemu_input_queue_scroll(s->con, INPUT_AXIS_Y, scroll->dy, phase,
                            momentum);
    qemu_input_queue_scroll(s->con, INPUT_AXIS_X, scroll->dx, phase,
                            momentum);
    qemu_input_event_sync();
}

static void reims_graphics_input_key(ReimsGraphicsPCIState *s, uint16_t usage,
                                     bool down)
{
    unsigned int lnx = 0;

    if (usage < qemu_input_map_usb_to_linux_len) {
        lnx = qemu_input_map_usb_to_linux[usage];
    }
    if (lnx != 0) {
        qemu_input_event_send_key_linux(s->con, lnx, down);
    }
}

/* ------ The guest's cursor, on the console ------ */

/*
 * The console's cursor place and whether it shows. The library gives the
 * place of the hot spot; QEMU's displays take the image's top-left corner,
 * as virtio-gpu gives it, and SPICE adds the hot spot back.
 */
static void reims_graphics_cursor_update(ReimsGraphicsPCIState *s)
{
    RvgDisplayCoord at;
    int hot_x = 0;
    int hot_y = 0;

    assert(bql_locked());
    if (!s->con || !s->device) {
        return;
    }
    at = rvg_graphics_device_cursor_position(s->device);
    if (s->cursor) {
        hot_x = s->cursor->hot_x;
        hot_y = s->cursor->hot_y;
    }
    qemu_console_set_mouse(s->con, at.x - hot_x, at.y - hot_y,
                           s->cursor_show && s->cursor);
}

static void reims_graphics_cursor_glyph(ReimsGraphicsPCIState *s,
                                        ReimsGraphicsEvent *event)
{
    const uint8_t *px = event->glyph.rgba;
    size_t count = (size_t)event->glyph.width * event->glyph.height;
    QEMUCursor *cursor;

    if (!s->con) {
        return;
    }
    cursor = cursor_alloc(event->glyph.width, event->glyph.height);
    if (!cursor) {
        return;
    }
    cursor->hot_x = event->glyph.hot_spot.x;
    cursor->hot_y = event->glyph.hot_spot.y;
    /* QEMUCursor's pixels are 0xAARRGGBB words. */
    for (size_t i = 0; i < count; i++, px += 4) {
        cursor->data[i] = ((uint32_t)px[3] << 24) | ((uint32_t)px[0] << 16) |
                          ((uint32_t)px[1] << 8) | px[2];
    }
    cursor_unref(s->cursor);
    s->cursor = cursor;
    qemu_console_set_cursor(s->con, cursor);
    reims_graphics_cursor_update(s);
}

static void reims_graphics_event_bh(void *opaque)
{
    ReimsGraphicsPCIState *s = opaque;
    ReimsGraphicsEvent *event;

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
        switch (event->kind) {
        case REIMS_GRAPHICS_EVENT_POINTER:
            reims_graphics_input_pointer(s, event);
            break;
        case REIMS_GRAPHICS_EVENT_SCROLL:
            reims_graphics_input_scroll(s, &event->scroll);
            break;
        case REIMS_GRAPHICS_EVENT_KEY:
            reims_graphics_input_key(s, event->key.usage, event->key.down);
            break;
        case REIMS_GRAPHICS_EVENT_CLOSE:
            /* As QEMU's own windows do: closing one powers the VM off. */
            shutdown_action = SHUTDOWN_ACTION_POWEROFF;
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            break;
        case REIMS_GRAPHICS_EVENT_CURSOR_GLYPH:
            reims_graphics_cursor_glyph(s, event);
            break;
        case REIMS_GRAPHICS_EVENT_CURSOR_SHOW:
            s->cursor_show = event->show;
            reims_graphics_cursor_update(s);
            break;
        case REIMS_GRAPHICS_EVENT_CURSOR_MOVE:
            reims_graphics_cursor_update(s);
            break;
        default:
            g_assert_not_reached();
        }
        reims_graphics_event_free(event);
    }
}

/*
 * Queues @event behind every earlier one, on the window's thread (on macOS
 * the process's main thread, which QEMU leaves in its run loop under
 * -display none, and on Linux the library's reims-vgpu.ui thread) or on the
 * library's display thread, for the cursor. None of them holds the BQL, and
 * the window's may not block, so the BH runs the input and the cursor.
 */
static void reims_graphics_event_push(ReimsGraphicsPCIState *s,
                                      ReimsGraphicsEvent *event)
{
    WITH_QEMU_LOCK_GUARD(&s->event_lock) {
        QSIMPLEQ_INSERT_TAIL(&s->events, event, next);
    }
    qemu_bh_schedule(s->event_bh);
}

static ReimsGraphicsEvent *reims_graphics_event_new(ReimsGraphicsEventKind kind)
{
    ReimsGraphicsEvent *event = g_new0(ReimsGraphicsEvent, 1);

    event->kind = kind;
    return event;
}

static void reims_graphics_window_pointer_cb(void *ctx, uint16_t x, uint16_t y,
                                             uint16_t width, uint16_t height,
                                             uint32_t buttons)
{
    ReimsGraphicsEvent *event =
        reims_graphics_event_new(REIMS_GRAPHICS_EVENT_POINTER);

    event->pointer.x = x;
    event->pointer.y = y;
    event->pointer.width = width;
    event->pointer.height = height;
    event->pointer.buttons = buttons;
    reims_graphics_event_push(ctx, event);
}

static void reims_graphics_window_scroll_cb(void *ctx,
                                            const RvgWindowScroll *scroll)
{
    ReimsGraphicsEvent *event =
        reims_graphics_event_new(REIMS_GRAPHICS_EVENT_SCROLL);

    /* The fields the library's version has; the rest stay 0. */
    memcpy(&event->scroll, scroll,
           MIN(scroll->struct_size, sizeof(event->scroll)));
    reims_graphics_event_push(ctx, event);
}

/* usage is a USB HID keyboard usage (page 0x07). */
static void reims_graphics_window_key_cb(void *ctx, uint16_t usage, bool down)
{
    ReimsGraphicsEvent *event =
        reims_graphics_event_new(REIMS_GRAPHICS_EVENT_KEY);

    event->key.usage = usage;
    event->key.down = down;
    reims_graphics_event_push(ctx, event);
}

static void reims_graphics_window_close_cb(void *ctx)
{
    ReimsGraphicsEvent *event =
        reims_graphics_event_new(REIMS_GRAPHICS_EVENT_CLOSE);

    reims_graphics_event_push(ctx, event);
}

/* The cursor's callbacks, on the library's display thread. */
static void reims_graphics_cursor_glyph_cb(void *ctx,
                                           const RvgCursorGlyph *glyph)
{
    ReimsGraphicsEvent *event =
        reims_graphics_event_new(REIMS_GRAPHICS_EVENT_CURSOR_GLYPH);
    /* height rows of width * 4 bytes */
    size_t length = (size_t)glyph->width * 4 * glyph->height;

    event->glyph.width = glyph->width;
    event->glyph.height = glyph->height;
    event->glyph.hot_spot = glyph->hot_spot;
    event->glyph.rgba = g_malloc0(length);
    memcpy(event->glyph.rgba, glyph->rgba, MIN(glyph->rgba_length, length));
    reims_graphics_event_push(ctx, event);
}

/* show is the library's byte: any value but 0 shows the cursor. */
static void reims_graphics_cursor_show_cb(void *ctx, uint8_t show)
{
    ReimsGraphicsEvent *event =
        reims_graphics_event_new(REIMS_GRAPHICS_EVENT_CURSOR_SHOW);

    event->show = show != 0;
    reims_graphics_event_push(ctx, event);
}

static void reims_graphics_cursor_move_cb(void *ctx)
{
    reims_graphics_event_push(
        ctx, reims_graphics_event_new(REIMS_GRAPHICS_EVENT_CURSOR_MOVE));
}

/*
 * The window's first size, from the resolution property: NULL is 0x0, which
 * the library opens at 1920x1080, else "<width>x<height>", each 1 to 65535
 * pixels.
 */
static bool reims_graphics_parse_resolution(const char *str, uint16_t *width,
                                            uint16_t *height, Error **errp)
{
    const char *endptr;
    int w, h;

    *width = 0;
    *height = 0;
    if (str == NULL) {
        return true;
    }
    if (qemu_strtoi(str, &endptr, 10, &w) || w <= 0 || w > UINT16_MAX ||
        *endptr != 'x' ||
        qemu_strtoi(endptr + 1, &endptr, 10, &h) || h <= 0 ||
        h > UINT16_MAX || *endptr != '\0') {
        error_setg(errp, "%s: resolution must be <width>x<height>, each 1 to "
                   "65535 pixels, not '%s'", TYPE_REIMS_GRAPHICS_PCI, str);
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

/*
 * Window mode: under -display none, QEMU shows no display of its own, and the
 * library opens a window for the display, unless REIMS_VGPU_WINDOW is "0".
 */
static bool reims_graphics_window_wanted(void)
{
    DisplayOptions *options = qmp_query_display_options(&error_abort);
    bool window = options->type == DISPLAY_TYPE_NONE &&
                  g_strcmp0(g_getenv("REIMS_VGPU_WINDOW"), "0") != 0;

    qapi_free_DisplayOptions(options);
    return window;
}

/* ------ The registers ------ */

/*
 * The vCPU calls the library itself: an access waits at most for the command
 * the control thread is taking, which waits for nothing of QEMU's, so it
 * needs no BQL. A DOORBELL write of a command queue's ring may answer the
 * queue's next command buffer on the vCPU and raise the interrupt there,
 * waiting for nothing and allocating nothing.
 */
static uint64_t reims_graphics_pci_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    ReimsGraphicsPCIState *s = opaque;

    return rvg_graphics_device_mmio_read(s->device, offset);
}

static void reims_graphics_pci_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    ReimsGraphicsPCIState *s = opaque;

    rvg_graphics_device_mmio_write_taking(s->device, offset, value);
}

static const MemoryRegionOps reims_graphics_pci_ops = {
    .read = reims_graphics_pci_read,
    .write = reims_graphics_pci_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* ------ Initialisation and startup ------ */

/* ------ The console: what the display shows, read back ------ */

typedef struct ReimsGraphicsReadJob {
    ReimsGraphicsPCIState *s;
    RvgGraphicsDevice *device;
    /* length bytes; the read fills height rows of width * 4, x8r8g8b8 */
    uint8_t *pixels;
    size_t length;
    /* The display's mode when the read ran */
    uint32_t width;
    uint32_t height;
    RvgStatus status;
} ReimsGraphicsReadJob;

static void reims_graphics_pci_read_done_bh(void *opaque);

static int reims_graphics_pci_do_read(void *opaque)
{
    ReimsGraphicsReadJob *job = opaque;

    /*
     * This runs on a pool thread, not under the BQL: the read waits for its
     * draw on the GPU. The library gives the mode's size whatever it answers,
     * so a buffer too small for the mode is made its size, and the read runs
     * once more.
     */
    job->status = rvg_graphics_device_read_display(job->device, job->pixels,
                                                   job->length, &job->width,
                                                   &job->height);
    if (job->status == RVG_ERR_INVALID_ARGUMENT &&
        (size_t)job->width * job->height * 4 > job->length) {
        job->length = (size_t)job->width * job->height * 4;
        job->pixels = g_realloc(job->pixels, job->length);
        job->status = rvg_graphics_device_read_display(job->device,
                                                       job->pixels,
                                                       job->length,
                                                       &job->width,
                                                       &job->height);
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            reims_graphics_pci_read_done_bh, job);
    return 0;
}

static void reims_graphics_pci_read_done_bh(void *opaque)
{
    ReimsGraphicsReadJob *job = opaque;
    ReimsGraphicsPCIState *s = job->s;
    DisplaySurface *surface = s->surface;
    size_t stride = (size_t)job->width * 4;
    uint32_t y;

    assert(bql_locked());
    s->reading = false;
    if (job->status != RVG_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: rvg_graphics_device_read_display: %s\n",
                      TYPE_REIMS_GRAPHICS_PCI,
                      rvg_status_message(job->status));
    } else if (s->con) {
        if (!surface || surface_width(surface) != job->width ||
            surface_height(surface) != job->height) {
            /* The console frees the surface it had. */
            surface = qemu_create_displaysurface(job->width, job->height);
            s->surface = surface;
            qemu_console_set_surface(s->con, surface);
        }
        /* The pixels have the surface's size and pixman's x8r8g8b8 layout. */
        for (y = 0; y < job->height; y++) {
            memcpy((uint8_t *)surface_data(surface) +
                   (size_t)y * surface_stride(surface),
                   job->pixels + y * stride, stride);
        }
        qemu_console_update_full(s->con);
    }
    g_free(s->pixels);
    s->pixels = job->pixels;
    s->pixels_length = job->length;
    g_free(job);
    /*
     * The update gfx_update asked for, if one did, read or not: an update
     * that waits for it, a screendump's, never waits for another.
     */
    qemu_console_hw_update_done(s->con);
}

/*
 * QEMU's display system asks for the surface: a display change listener's
 * refresh, or a screendump. A read of the display starts, unless one is
 * under way, and signals the update done; it reads what the display shows
 * when it runs.
 */
static bool reims_graphics_pci_gfx_update(void *opaque)
{
    ReimsGraphicsPCIState *s = opaque;
    ReimsGraphicsReadJob *job;

    assert(bql_locked());
    if (s->reading) {
        return false;
    }
    if (!s->device || !s->pool) {
        return true;
    }
    s->reading = true;
    job = g_new0(ReimsGraphicsReadJob, 1);
    job->s = s;
    job->device = s->device;
    job->pixels = s->pixels;
    job->length = s->pixels_length;
    s->pixels = NULL;
    s->pixels_length = 0;
    thread_pool_submit_immediate(s->pool, reims_graphics_pci_do_read, job,
                                 NULL);
    return false;
}

static const GraphicHwOps reims_graphics_pci_console_ops = {
    .gfx_update = reims_graphics_pci_gfx_update,
};

static void reims_graphics_pci_init(Object *obj)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(obj);

    memory_region_init(&s->bar0, obj, "reims-graphics-pci.bar0",
                       RVG_GRAPHICS_REGISTERS_SIZE);
    memory_region_init_io(&s->registers, obj, &reims_graphics_pci_ops, s,
                          "reims-graphics-pci.registers",
                          RVG_GRAPHICS_MSIX_OFFSET);
    memory_region_enable_lockless_io(&s->registers);
    memory_region_add_subregion(&s->bar0, 0, &s->registers);
    qemu_mutex_init(&s->event_lock);
    QSIMPLEQ_INIT(&s->events);
}

/*
 * Undoes what realize did, from its failure or at unrealize, with the BARs
 * unmapped. The library's threads end without being waited for, and its
 * region stays mapped until they and the window's GPU work let it go. A
 * callback they or the window run meanwhile still schedules its BH, or
 * writes the irqfd, which stays open for it: the device
 * is not hotpluggable, so that is only at QEMU's end.
 */
static void reims_graphics_pci_teardown(ReimsGraphicsPCIState *s)
{
    PCIDevice *dev = PCI_DEVICE(s);
    ReimsGraphicsEvent *event;

    if (s->con) {
        qemu_graphic_console_close(s->con);
        s->con = NULL;
        s->surface = NULL;
    }
    /* A read under way ends first: it uses the device. */
    if (s->pool) {
        thread_pool_free(s->pool);
        s->pool = NULL;
    }
    g_free(s->pixels);
    s->pixels = NULL;
    s->pixels_length = 0;
    if (s->doorbell_added) {
        memory_region_del_eventfd(&s->registers, RVG_GRAPHICS_REG_DOORBELL,
                                  4, true, RVG_GRAPHICS_DOORBELL_CONTROL,
                                  &s->doorbell);
        s->doorbell_added = false;
    }
    if (s->doorbell_made) {
        event_notifier_cleanup(&s->doorbell);
        s->doorbell_made = false;
    }
    if (s->irqfd_made) {
        reims_graphics_pci_irqfd_disable(s);
    }
    if (msix_present(dev)) {
        msix_uninit(dev, &s->bar0, &s->bar0);
    }
    rvg_graphics_device_release(s->device);
    s->device = NULL;
    if (s->event_bh) {
        qemu_bh_delete(s->event_bh);
        s->event_bh = NULL;
    }
    WITH_QEMU_LOCK_GUARD(&s->event_lock) {
        while ((event = QSIMPLEQ_FIRST(&s->events)) != NULL) {
            QSIMPLEQ_REMOVE_HEAD(&s->events, next);
            reims_graphics_event_free(event);
        }
    }
    cursor_unref(s->cursor);
    s->cursor = NULL;
}

typedef struct ReimsGraphicsMemoryWalk {
    GArray *ranges;
    GArray *files;
    bool every_range_has_a_file;
} ReimsGraphicsMemoryWalk;

/*
 * One range of the guest's RAM, and the file behind it: a shared mapping of
 * a file in pages of the host's size, which the library's views of an
 * IOSurface's pages map again. Any other RAM (anonymous, private or huge
 * pages) has none.
 */
static bool reims_graphics_add_memory_range(Int128 start, Int128 len,
                                            const MemoryRegion *mr,
                                            hwaddr offset_in_region,
                                            void *opaque)
{
    ReimsGraphicsMemoryWalk *walk = opaque;
    RAMBlock *rb = mr->ram_block;
    void *host;
    RvgMemoryRange range;
    RvgMemoryRangeFile file = { .fd = -1 };
    int fd;

    if (!memory_access_is_direct(mr, true, MEMTXATTRS_UNSPECIFIED)) {
        return false;
    }
    /* q35's RAM above 4 GiB is an alias of the block past the part below. */
    host = (uint8_t *)memory_region_get_ram_ptr(mr) + offset_in_region;
    range = (RvgMemoryRange) {
        .physical_address = int128_get64(start),
        .physical_length = int128_get64(len),
        .host_address = host,
    };
    fd = memory_region_get_fd(mr);
    if (fd >= 0 && qemu_ram_is_shared(rb) &&
        qemu_ram_pagesize(rb) == qemu_real_host_page_size()) {
        file.fd = fd;
        file.offset = qemu_ram_block_host_offset(rb, host) +
                      qemu_ram_get_fd_offset(rb);
    } else {
        walk->every_range_has_a_file = false;
    }
    g_array_append_val(walk->ranges, range);
    g_array_append_val(walk->files, file);
    return false;
}

/*
 * Warns where the machine leaves BAR 2 no address in a macOS guest. macOS
 * lays every 64-bit prefetchable BAR out below 4 GiB, where the region's
 * 1 GiB needs the window to start at or below 1 GiB: a PC machine that
 * keeps more RAM than that below 4 GiB, as q35 does by default, has RAM at
 * 1 GiB, and the guest's kext then finds no region. The host keeps 1 GiB of
 * RAM below 4 GiB with -machine max-ram-below-4g=1G (docs/qemu.md). Only a
 * PC machine has that property; the device is realized either way.
 */
static void reims_graphics_pci_check_low_ram(void)
{
    MemoryRegionSection section;
    bool ram;

    if (!object_property_find(qdev_get_machine(), "max-ram-below-4g")) {
        return;
    }
    section = memory_region_find(get_system_memory(), 1 * GiB, 1);
    if (!section.mr) {
        return;
    }
    ram = memory_region_is_ram(section.mr);
    memory_region_unref(section.mr);
    if (ram) {
        warn_report("%s: the machine has RAM at 1 GiB, so a macOS guest gives "
                    "BAR 2 (1 GiB, below 4 GiB) no address and its kext finds "
                    "no region: start QEMU with "
                    "-machine max-ram-below-4g=1G",
                    TYPE_REIMS_GRAPHICS_PCI);
    }
}

static void reims_graphics_pci_realize(PCIDevice *dev, Error **errp)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(dev);
    RvgGraphicsCallbacks callbacks = {
        .struct_size = sizeof(callbacks),
        .ctx = s,
        .raise_interrupt = reims_graphics_pci_raise_interrupt_cb,
        .cursor_glyph = reims_graphics_cursor_glyph_cb,
        .cursor_show = reims_graphics_cursor_show_cb,
        .cursor_move = reims_graphics_cursor_move_cb,
    };
    RvgWindowCallbacks window_callbacks = {
        .struct_size = sizeof(window_callbacks),
        .ctx = s,
        .pointer = reims_graphics_window_pointer_cb,
        .key = reims_graphics_window_key_cb,
        .close = reims_graphics_window_close_cb,
        .scroll = reims_graphics_window_scroll_cb,
    };
    /* The library opens the window at the mode, and ignores the flags. */
    RvgWindowConfig window_config = {
        .struct_size = sizeof(window_config),
        .callbacks = &window_callbacks,
    };
    RvgGraphicsDeviceConfig config = {
        .struct_size = sizeof(config),
        .callbacks = &callbacks,
        .window = reims_graphics_window_wanted() ? &window_config : NULL,
    };
    g_autoptr(GArray) ranges = g_array_new(false, false,
                                           sizeof(RvgMemoryRange));
    g_autoptr(GArray) files = g_array_new(false, false,
                                          sizeof(RvgMemoryRangeFile));
    ReimsGraphicsMemoryWalk walk = {
        .ranges = ranges,
        .files = files,
        .every_range_has_a_file = true,
    };
    RvgMemoryMap memory_map;
    RvgStatus status;
    uint8_t *base;
    uint64_t size;

    if (!reims_graphics_parse_resolution(s->resolution,
                                         &window_config.mode_width,
                                         &window_config.mode_height, errp)) {
        return;
    }
    reims_graphics_pci_check_low_ram();

    if (reims_graphics_mig_blocker == NULL) {
        error_setg(&reims_graphics_mig_blocker,
                   "Migration state blocked by reims-graphics-pci");
        if (migrate_add_blocker(&reims_graphics_mig_blocker, errp) < 0) {
            return;
        }
    }

    /*
     * The guest's RAM, through which the GPU uses an IOSurface's own pages in
     * place. The library's views of them need the file behind each range; RAM
     * without one (not a shared memfd, or huge pages) gets no map, and each
     * surface's GPU view stays a copy of its pages in the region. The library
     * keeps views of the map's pages while the device lives, so they must stay
     * the guest's RAM: no balloon or hot-unplug may discard them.
     */
    flatview_for_each_range(address_space_to_flatview(&address_space_memory),
                            reims_graphics_add_memory_range, &walk);
    if (walk.ranges->len > 0 && walk.every_range_has_a_file) {
        memory_map = (RvgMemoryMap) {
            .ranges = (const RvgMemoryRange *)walk.ranges->data,
            .range_count = walk.ranges->len,
        };
        config.memory_map = &memory_map;
        config.memory_files = (const RvgMemoryRangeFile *)walk.files->data;
    } else {
        warn_report("%s: guest RAM is not a shared memfd in pages of the "
                    "host's size: IOSurfaces are copied for the GPU",
                    TYPE_REIMS_GRAPHICS_PCI);
    }

    /* The window's input runs from here on; the callbacks need the BH. */
    s->event_bh = qemu_bh_new(reims_graphics_event_bh, s);
    status = rvg_graphics_device_create(&config, &s->device);
    if (status != RVG_OK) {
        error_setg(errp, "%s: rvg_graphics_device_create: %s",
                   TYPE_REIMS_GRAPHICS_PCI, rvg_status_message(status));
        goto fail;
    }

    if (msix_init(dev, RVG_GRAPHICS_MSIX_VECTORS, &s->bar0,
                  RVG_GRAPHICS_PCI_BAR_REGISTERS, RVG_GRAPHICS_MSIX_OFFSET,
                  &s->bar0, RVG_GRAPHICS_PCI_BAR_REGISTERS,
                  REIMS_GRAPHICS_PCI_PBA_OFFSET, 0, errp) < 0) {
        goto fail;
    }
    /* The one vector is always in use: the guest has only it. */
    msix_vector_use(dev, 0);
    s->irqfd_virq = -1;
    if (accel_msi_via_irqfd_enabled()) {
        if (event_notifier_init(&s->irqfd, false) < 0) {
            error_setg(errp, "%s: the interrupt's event notifier",
                       TYPE_REIMS_GRAPHICS_PCI);
            goto fail;
        }
        s->irqfd_made = true;
    }
    pci_register_bar(dev, RVG_GRAPHICS_PCI_BAR_REGISTERS,
                     PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    if (event_notifier_init(&s->doorbell, false) < 0) {
        error_setg(errp, "%s: the doorbell's event notifier",
                   TYPE_REIMS_GRAPHICS_PCI);
        goto fail;
    }
    s->doorbell_made = true;
    status = rvg_graphics_device_set_doorbell(
        s->device, event_notifier_get_fd(&s->doorbell));
    if (status != RVG_OK) {
        error_setg(errp, "%s: rvg_graphics_device_set_doorbell: %s",
                   TYPE_REIMS_GRAPHICS_PCI, rvg_status_message(status));
        goto fail;
    }
    memory_region_add_eventfd(&s->registers, RVG_GRAPHICS_REG_DOORBELL, 4,
                              true, RVG_GRAPHICS_DOORBELL_CONTROL,
                              &s->doorbell);
    s->doorbell_added = true;

    base = rvg_graphics_device_region(s->device, &size);
    memory_region_init_ram_ptr(&s->region, OBJECT(dev),
                               "reims-graphics-pci.region", size, base);
    pci_register_bar(dev, RVG_GRAPHICS_PCI_BAR_REGION,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->region);

    /*
     * The source of the window's input, the head QEMU's UI names, and what
     * the display shows, which its reads run in the pool for.
     */
    s->pool = thread_pool_new();
    s->con = qemu_graphic_console_create(DEVICE(dev), 0,
                                         &reims_graphics_pci_console_ops, s);
    return;

fail:
    reims_graphics_pci_teardown(s);
}

static void reims_graphics_pci_exit(PCIDevice *dev)
{
    reims_graphics_pci_teardown(REIMS_GRAPHICS_PCI(dev));
}

static void reims_graphics_pci_reset(Object *obj, ResetType type)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(obj);

    /* The reset disables MSI-X without a config write. */
    if (s->irqfd_made) {
        reims_graphics_pci_irqfd_disable(s);
    }
    if (s->device) {
        rvg_graphics_device_reset(s->device);
    }
}

static void reims_graphics_pci_finalize(Object *obj)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(obj);

    qemu_mutex_destroy(&s->event_lock);
}

static const Property reims_graphics_pci_properties[] = {
    DEFINE_PROP_STRING("resolution", ReimsGraphicsPCIState, resolution),
};

static void reims_graphics_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pci = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = reims_graphics_pci_reset;
    dc->desc = "reims-graphics GPU";
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_props(dc, reims_graphics_pci_properties);

    pci->vendor_id = RVG_GRAPHICS_PCI_VENDOR_ID;
    pci->device_id = RVG_GRAPHICS_PCI_DEVICE_ID;
    pci->class_id = RVG_GRAPHICS_PCI_CLASS;
    pci->realize = reims_graphics_pci_realize;
    pci->config_write = reims_graphics_pci_write_config;
    pci->exit = reims_graphics_pci_exit;
}

static const TypeInfo reims_graphics_pci_types[] = {
    {
        .name          = TYPE_REIMS_GRAPHICS_PCI,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(ReimsGraphicsPCIState),
        .class_init    = reims_graphics_pci_class_init,
        .instance_init = reims_graphics_pci_init,
        .instance_finalize = reims_graphics_pci_finalize,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { },
        },
    },
};
DEFINE_TYPES(reims_graphics_pci_types)
