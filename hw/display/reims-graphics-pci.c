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
 *    ioeventfd, which the library's control thread polls, so ringing never
 *    runs the library on the vCPU.
 *  - BAR 2: the library's region, 64-bit and prefetchable, private anonymous
 *    memory of QEMU's process that the guest maps write-back: the control
 *    rings below RVG_GRAPHICS_ALLOCATIONS_OFFSET, the guest's allocations
 *    from there. It is two RAM regions over the one mapping, so that dirty
 *    logging covers only the allocations, where the guest's scanout lies,
 *    and the rings' constant writes are never logged.
 *  - The library's callbacks run on its own threads with none of its locks
 *    held: an interrupt is raised by a BH, and scanout_written takes the BQL
 *    to read and clear the allocations' dirty log.
 *
 * Under -display none the library opens a window for the display, unless
 * REIMS_VGPU_WINDOW is "0", as apple-gfx does: the window keeps the size the
 * person gives it, and the guest follows it. Its input reaches the guest
 * through the device's console. QEMU's own displays show nothing of the
 * device.
 *
 * The device has no default option ROM: romfile names one.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/event_notifier.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/queue.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-ui.h"
#include "migration/blocker.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
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
    MemoryRegion rings;     /* in BAR 2: the control rings */
    MemoryRegion allocations; /* in BAR 2: the guest's, logged */
    EventNotifier doorbell;
    bool doorbell_made;  /* the notifier is initialised */
    bool doorbell_added; /* the notifier is BAR 0's ioeventfd */
    RvgGraphicsDevice *device;
    QemuConsole *con;

    /* The window's input, queued on its thread and run under the BQL. */
    QemuMutex event_lock;
    QSIMPLEQ_HEAD(, ReimsGraphicsEvent) events; /* event_lock protected */
    QEMUBH *event_bh;

    /* Mutable state (BQL protected) */
    uint32_t input_buttons; /* RVG_WINDOW_BUTTON_* bits the guest was sent */
};

static Error *reims_graphics_mig_blocker;

/* ------ Interrupts and the dirty log ------ */

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

/* On the library's control or refresh thread, never inside a call of ours. */
static void reims_graphics_pci_raise_interrupt_cb(void *ctx, uint32_t vector)
{
    ReimsGraphicsInterruptJob *job = g_new0(ReimsGraphicsInterruptJob, 1);

    job->device = ctx;
    job->vector = vector;
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            reims_graphics_pci_raise_interrupt, job);
}

/*
 * On the library's refresh thread, once a refresh while the guest has a
 * scanout: whether the guest wrote any of its bytes since the last call. The
 * range lies in the allocations, which alone are logged.
 */
static bool reims_graphics_pci_scanout_written_cb(void *ctx, uint64_t offset,
                                                  uint64_t length)
{
    ReimsGraphicsPCIState *s = ctx;
    DirtyBitmapSnapshot *snap;
    hwaddr start = offset - RVG_GRAPHICS_ALLOCATIONS_OFFSET;
    bool written;

    BQL_LOCK_GUARD();
    snap = memory_region_snapshot_and_clear_dirty(&s->allocations, start,
                                                  length, DIRTY_MEMORY_VGA);
    written = memory_region_snapshot_get_dirty(&s->allocations, snap, start,
                                               length);
    g_free(snap);
    return written;
}

/* ------ Input from the display's window ------ */

typedef enum ReimsGraphicsEventKind {
    REIMS_GRAPHICS_EVENT_POINTER,
    REIMS_GRAPHICS_EVENT_SCROLL,
    REIMS_GRAPHICS_EVENT_KEY,
    REIMS_GRAPHICS_EVENT_CLOSE,
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
    };
};

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
        default:
            g_assert_not_reached();
        }
        g_free(event);
    }
}

/*
 * Queues @event behind every earlier one, on the window's thread: on macOS
 * the process's main thread, which QEMU leaves in its run loop under
 * -display none, and on Linux the library's reims-vgpu.ui thread. Neither
 * holds the BQL, and neither may block, so the BH runs the input.
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
 * needs no BQL.
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

    rvg_graphics_device_mmio_write(s->device, offset, value);
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

/* QEMU's displays show nothing of the device: its console only has input. */
static const GraphicHwOps reims_graphics_pci_console_ops = {};

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
 * callback they or the window run meanwhile still schedules its BH, as
 * apple-gfx's do: the device is not hotpluggable, so that is only at QEMU's
 * end.
 */
static void reims_graphics_pci_teardown(ReimsGraphicsPCIState *s)
{
    PCIDevice *dev = PCI_DEVICE(s);
    ReimsGraphicsEvent *event;

    if (s->con) {
        qemu_graphic_console_close(s->con);
        s->con = NULL;
    }
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
            g_free(event);
        }
    }
}

static void reims_graphics_pci_realize(PCIDevice *dev, Error **errp)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(dev);
    RvgGraphicsCallbacks callbacks = {
        .struct_size = sizeof(callbacks),
        .ctx = s,
        .raise_interrupt = reims_graphics_pci_raise_interrupt_cb,
        .scanout_written = reims_graphics_pci_scanout_written_cb,
    };
    RvgWindowCallbacks window_callbacks = {
        .struct_size = sizeof(window_callbacks),
        .ctx = s,
        .pointer = reims_graphics_window_pointer_cb,
        .key = reims_graphics_window_key_cb,
        .close = reims_graphics_window_close_cb,
        .scroll = reims_graphics_window_scroll_cb,
    };
    RvgWindowConfig window_config = {
        .struct_size = sizeof(window_config),
        .callbacks = &window_callbacks,
    };
    RvgGraphicsDeviceConfig config = {
        .struct_size = sizeof(config),
        .callbacks = &callbacks,
        .window = reims_graphics_window_wanted() ? &window_config : NULL,
    };
    RvgStatus status;
    uint8_t *base;
    uint64_t size;

    if (reims_graphics_mig_blocker == NULL) {
        error_setg(&reims_graphics_mig_blocker,
                   "Migration state blocked by reims-graphics-pci");
        if (migrate_add_blocker(&reims_graphics_mig_blocker, errp) < 0) {
            return;
        }
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
    memory_region_init(&s->region, OBJECT(dev), "reims-graphics-pci.region",
                       size);
    memory_region_init_ram_ptr(&s->rings, OBJECT(dev),
                               "reims-graphics-pci.rings",
                               RVG_GRAPHICS_ALLOCATIONS_OFFSET, base);
    memory_region_init_ram_ptr(&s->allocations, OBJECT(dev),
                               "reims-graphics-pci.allocations",
                               size - RVG_GRAPHICS_ALLOCATIONS_OFFSET,
                               base + RVG_GRAPHICS_ALLOCATIONS_OFFSET);
    memory_region_add_subregion(&s->region, 0, &s->rings);
    memory_region_add_subregion(&s->region, RVG_GRAPHICS_ALLOCATIONS_OFFSET,
                                &s->allocations);
    memory_region_set_log(&s->allocations, true, DIRTY_MEMORY_VGA);
    pci_register_bar(dev, RVG_GRAPHICS_PCI_BAR_REGION,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->region);

    /* The source of the window's input, and the head QEMU's UI names. */
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

    if (s->device) {
        rvg_graphics_device_reset(s->device);
    }
}

static void reims_graphics_pci_finalize(Object *obj)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(obj);

    qemu_mutex_destroy(&s->event_lock);
}

static void reims_graphics_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pci = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = reims_graphics_pci_reset;
    dc->desc = "reims-graphics GPU";
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);

    pci->vendor_id = RVG_GRAPHICS_PCI_VENDOR_ID;
    pci->device_id = RVG_GRAPHICS_PCI_DEVICE_ID;
    pci->class_id = RVG_GRAPHICS_PCI_CLASS;
    pci->realize = reims_graphics_pci_realize;
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
