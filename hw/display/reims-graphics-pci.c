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
 *    ioeventfd, which the library's control thread polls (on Windows QEMU's
 *    event, which it waits on), so ringing the control ring never runs the
 *    library on the vCPU. So is a write of a
 *    command queue's ring the library routes (route_doorbell): an ioeventfd
 *    matching the ring's id signals the queue's own eventfd, which its thread
 *    polls. Any other write of a queue's ring goes to the library on the vCPU,
 *    which may answer the queue's next command buffer there, posting the
 *    event that says it is submitted, and leave the buffer to the queue's
 *    thread (rvg_graphics_device_mmio_write_taking).
 *  - BAR 4: the library's doorbell pages, 32-bit and not prefetchable,
 *    RVG_GRAPHICS_DOORBELLS_SIZE bytes of RVG_GRAPHICS_DOORBELL_PAGES pages.
 *    The guest's kext binds a page to each process it opens and maps the
 *    page into it, so the process rings its queues there with no call into
 *    the kext. A write goes to the library on the vCPU
 *    (rvg_graphics_device_doorbell_write), which wakes only a ring of the
 *    page's process and drops the rest; a read answers 0. A plain write of a
 *    queue's ring the library routes on a page (route_doorbell_page) is taken
 *    by an ioeventfd at the page's doorbell, as on DOORBELL.
 *  - BAR 2: the library's region, 64-bit and prefetchable, memory of QEMU's
 *    process (on Linux a shared mapping of a memfd of the library's, on macOS
 *    private anonymous memory, on Windows a view of a section of the
 *    library's) that the guest maps write-back: the control
 *    rings below RVG_GRAPHICS_ALLOCATIONS_OFFSET, the guest's allocations
 *    from there. It is one RAM region over the mapping, with no dirty log:
 *    the library's window shows the guest's scanout again at each of the
 *    display's refreshes while it shows no presented surface.
 *  - The guest's Shared pool: guest physical addresses with no RAM under
 *    them, from the host bridge's pci-hole64-end, as many as the library
 *    takes for the host GPU's Shared memory heap
 *    (rvg_graphics_device_pool_range), which the device reserves at realize;
 *    the firmware places its 64-bit PCI window past them
 *    (etc/reserved-memory-end). The library maps its blocks of host memory
 *    into the range as the guest's pool grows and takes them out as it
 *    shrinks, each its own RAM region, added and removed on the main loop
 *    ("The guest's Shared pool", below).
 *  - The library's callbacks run on its own threads, and raise_interrupt
 *    also on the vCPU inside that write, with no lock held that a call of
 *    ours into the library waits for. An interrupt is raised by a write of
 *    the vector's irqfd, where the accelerator injects MSI-X through one
 *    (KVM's in-kernel irqchip) and MSI-X is enabled; by a request to the
 *    hypervisor's APIC on the raising thread, where the accelerator has no
 *    irqfds but its APIC is the hypervisor's (WHPX's kernel-irqchip) and the
 *    enabled vector is unmasked; and by a BH otherwise. Neither the irqfd's
 *    write nor the request waits for the BQL or the main loop, and the lock
 *    the request takes is held for nothing that waits. The BH runs in the
 *    main loop, so a long turn of the main loop holds the interrupt back,
 *    and the guest takes no completion meanwhile. route_doorbell applies
 *    its change under the BQL before it returns, and the BQL's holders wait
 *    for none of the library's threads: the release at QEMU's quit runs
 *    with it let go.
 *
 * The device has the displays property's count of displays, 1 to
 * RVG_GRAPHICS_DISPLAYS_MAX (1 by default), display i head i of QEMU's UI,
 * with a console of its own. Under -display none the library opens a window
 * for each display, unless REIMS_VGPU_WINDOW is "0": each opens at the
 * resolution property's size, "<width>x<height>" (1920x1080 without it), then
 * keeps the size the person gives it, and the guest follows it. A window's
 * input reaches the guest through its display's console. A console shows what
 * its display shows, read back from the library on a thread of the device's
 * pool at each of its updates (rvg_graphics_device_read_display_at): QEMU's
 * own displays and its screendump of the console's head read it there. A
 * read waits for its draw on the GPU; one still waiting 3 s after it started
 * is late, and a screendump is answered then with what the console shows, so
 * that the QMP monitors, which wait for the screendump's command, go on. Each
 * console also has its display's guest cursor, which the library tells the
 * device on its display thread and a BH gives the console, so QEMU's own
 * displays (SPICE, VNC, GTK, D-Bus) draw it; the image read back has none,
 * and under -display none the console has no display, where the window shows
 * the cursor as the host's pointer.
 *
 * At QEMU's quit, once the VM stopped for good, the device is torn down
 * (reims_graphics_pci_vm_state_change): every pool piece leaves the guest's
 * address space, and the library's release of its last device lets go of
 * the GPU device before the process exits. A read of a
 * display still waiting for the GPU 5 s into the teardown keeps the device to
 * the exit instead, so a GPU that stopped answering does not hold the quit.
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
#include "qemu/target-info.h"
#include "qemu/timer.h"
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
#include "system/whpx.h"
#include "block/thread-pool.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/core/cpu.h"
#include "qapi/qapi-builtin-type-infos.h"
#include "qapi/visitor.h"
#include "qemu/rcu.h"
#include "trace.h"
#ifdef CONFIG_LINUX
#include "reims-graphics-pci-kvm.h"
#endif
#include "ui/console.h"
#include "ui/input.h"
#include <reims_vgpu.h>

#define TYPE_REIMS_GRAPHICS_PCI "reims-graphics-pci"
OBJECT_DECLARE_SIMPLE_TYPE(ReimsGraphicsPCIState, REIMS_GRAPHICS_PCI)

/* The PBA, after the table's one 16-byte entry, aligned as MSI-X needs. */
#define REIMS_GRAPHICS_PCI_PBA_OFFSET (RVG_GRAPHICS_MSIX_OFFSET + 0x400)

/* The most wheel notches one scroll event sends the guest, on each axis. */
#define REIMS_GRAPHICS_PCI_WHEEL_MAX_NOTCHES 256

/*
 * How long the teardown at QEMU's quit waits for the reads of the displays
 * under way, the bound the library's release of its last device waits for its
 * own threads.
 */
#define REIMS_GRAPHICS_PCI_READ_END_MS 5000

/*
 * How long after its start a read of a display under way is late: an update
 * (a screendump) that waits for it is answered then with what the display's
 * console shows. A read on a GPU that answers takes milliseconds; the bound
 * leaves a QMP client that waits 5 s for an answer (reims-ctl's viewer) 2 s of
 * margin.
 */
#define REIMS_GRAPHICS_PCI_READ_LATE_MS 3000

typedef struct ReimsGraphicsEvent ReimsGraphicsEvent;
typedef struct ReimsGraphicsPoolAsk ReimsGraphicsPoolAsk;

/*
 * A route applied: the ioeventfd matching `ring_id` at `addr` in `mr`, the
 * DOORBELL register or a doorbell page's doorbell.
 */
typedef struct ReimsGraphicsRoute {
    uint64_t route;
    uint32_t ring_id;
    MemoryRegion *mr;
    hwaddr addr;
    EventNotifier bell; /* over our copy of the queue's eventfd */
} ReimsGraphicsRoute;

/*
 * One of the device's displays, head `index` of QEMU's UI: its console, which
 * shows what the display shows, read back from the library, and its cursor.
 */
typedef struct ReimsGraphicsHead {
    ReimsGraphicsPCIState *s;
    uint32_t index;
    QemuConsole *con;
    bool read_running; /* s->read_lock protected: a read is in the library */

    /* Mutable state (BQL protected) */
    QEMUCursor *cursor;     /* the guest's cursor image, or NULL */
    bool cursor_show;       /* the library says the cursor shows */
    bool reading;           /* a read of the display is under way */
    bool read_late;         /* it ran past REIMS_GRAPHICS_PCI_READ_LATE_MS */
    QEMUTimer *read_bound;  /* fires that long after a read starts */
    DisplaySurface *surface; /* the console's, which the reads fill */
    uint8_t *pixels;        /* the last read's buffer, kept for the next */
    size_t pixels_length;
} ReimsGraphicsHead;

struct ReimsGraphicsPCIState {
    PCIDevice parent_obj;

    /* Initialised on init/realize() */
    MemoryRegion bar0;      /* BAR 0: the registers, the MSI-X table, PBA */
    MemoryRegion registers; /* in BAR 0, below the MSI-X table */
    MemoryRegion region;    /* BAR 2 */
    MemoryRegion doorbells; /* BAR 4: the doorbell pages */
    EventNotifier doorbell;
    bool doorbell_made;  /* the notifier is initialised */
    bool doorbell_added; /* the notifier is BAR 0's ioeventfd */
    /* The queues' doorbells the library routes (route_doorbell). */
    GHashTable *routes; /* BQL protected: route number -> ReimsGraphicsRoute */
    bool routes_closed; /* BQL protected: the teardown dropped them */
    EventNotifier irqfd; /* the vector's, where the accelerator has irqfds */
    bool irqfd_made;     /* the notifier is initialised */
    RvgGraphicsDevice *device;
    VMChangeStateEntry *vm_state; /* the teardown at QEMU's quit */
    /* The displays, display i head i: the first `displays` of them. */
    ReimsGraphicsHead heads[RVG_GRAPHICS_DISPLAYS_MAX];
    ThreadPool *pool; /* where the consoles' reads of the displays run */
    QemuMutex read_lock;
    QemuCond read_ended;

    /* The window's input, queued on its thread and run under the BQL. */
    QemuMutex event_lock;
    QSIMPLEQ_HEAD(, ReimsGraphicsEvent) events; /* event_lock protected */
    QEMUBH *event_bh;

    char *resolution; /* the property: each window's first size, or NULL */
    uint32_t displays; /* the property: 1 to RVG_GRAPHICS_DISPLAYS_MAX */

    /* Mutable state (BQL protected) */
    uint32_t input_buttons; /* RVG_WINDOW_BUTTON_* bits the guest was sent */
    int irqfd_virq;         /* the vector's MSI route while MSI-X is enabled */
    bool irqfd_unmasked;    /* the irqfd is the route's */
    /* Atomic, set under the BQL: whether a raise writes the irqfd. */
    bool irqfd_on;
    /*
     * Where the accelerator has no irqfds and its APIC is the hypervisor's
     * (WHPX), the vector's message while MSI-X is enabled, which a raise
     * delivers on its own thread (whpx_send_msi_direct).
     */
    bool msi_direct_made;   /* set at realize, msi_lock initialised */
    QemuMutex msi_lock;     /* held for no call of the library's */
    bool msi_direct_on;     /* BQL protected: the vector notifiers are ours */
    bool msi_unmasked;      /* msi_lock protected: the message is current */
    MSIMessage msi_message; /* msi_lock protected */

    /*
     * The guest's Shared pool: the range reserved for it at realize, and the
     * library's asks to map and unmap its pieces, queued on its threads and
     * carried out under the BQL (reims_graphics_pool_bh).
     */
    hwaddr pool_base;    /* where the range starts, once reserved */
    uint64_t pool_length; /* its length: 0 where the device has no pool */
    QemuMutex pool_lock;
    QSIMPLEQ_HEAD(, ReimsGraphicsPoolAsk) pool_asks; /* pool_lock protected */
    bool pool_closed;   /* pool_lock protected: the teardown took the pieces */
    QEMUBH *pool_bh;
    GPtrArray *pool_pieces; /* BQL protected: the pieces in the guest */
    int pool_alive;         /* BQL protected: pieces not yet finalized */
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
 * the library's write of what the interrupt reports: the irqfd's write, or
 * the hypervisor's request, follows it on the same thread after a full
 * barrier, so the guest takes the interrupt after it sees the write, as it
 * does through the BH. The request is made under msi_lock, which the vector's
 * mask and unmask take under the BQL and which waits for nothing else, so no
 * interrupt is requested once the guest's mask of the vector has returned. A
 * vector masked, MSI-X disabled, or a request the hypervisor refuses goes
 * through the BH, where msix_notify sets the pending bit, drops it or says
 * why it is lost, as before.
 */
static void reims_graphics_pci_raise_interrupt_cb(void *ctx, uint32_t vector)
{
    ReimsGraphicsPCIState *s = ctx;
    ReimsGraphicsInterruptJob *job;
    bool sent = false;

    if (qatomic_load_acquire(&s->irqfd_on)) {
        event_notifier_set(&s->irqfd);
        return;
    }
    if (s->msi_direct_made) {
        smp_mb();
        qemu_mutex_lock(&s->msi_lock);
        if (s->msi_unmasked) {
            sent = whpx_send_msi_direct(s->msi_message.address,
                                        s->msi_message.data);
        }
        qemu_mutex_unlock(&s->msi_lock);
        if (sent) {
            return;
        }
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
        /* The guest masks the vector at will: said once a process. */
        error_report_once("%s: remove_irqfd_notifier_gsi failed",
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

/*
 * The vector's message for the hypervisor's direct delivery, while MSI-X is
 * enabled: each unmask gives the message the table holds then, as the irqfd's
 * route takes it. A raise while the vector is masked goes through the BH,
 * whose msix_notify sets the pending bit that the unmask then delivers.
 */
static int reims_graphics_pci_direct_unmask(PCIDevice *dev, unsigned vector,
                                            MSIMessage msg)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(dev);

    WITH_QEMU_LOCK_GUARD(&s->msi_lock) {
        s->msi_message = msg;
        s->msi_unmasked = true;
    }
    return 0;
}

static void reims_graphics_pci_direct_mask(PCIDevice *dev, unsigned vector)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(dev);

    WITH_QEMU_LOCK_GUARD(&s->msi_lock) {
        s->msi_unmasked = false;
    }
}

static void reims_graphics_pci_direct_enable(ReimsGraphicsPCIState *s)
{
    PCIDevice *dev = PCI_DEVICE(s);

    if (msix_set_vector_notifiers(dev, reims_graphics_pci_direct_unmask,
                                  reims_graphics_pci_direct_mask,
                                  NULL) < 0) {
        error_report("%s: set_vector_notifiers failed: interrupts go "
                     "through a BH", TYPE_REIMS_GRAPHICS_PCI);
        return;
    }
    s->msi_direct_on = true;
}

/*
 * A raise between the guest's disable of MSI-X and this may still be
 * delivered, as one that writes the irqfd then is.
 */
static void reims_graphics_pci_direct_disable(ReimsGraphicsPCIState *s)
{
    if (!s->msi_direct_on) {
        return;
    }
    msix_unset_vector_notifiers(PCI_DEVICE(s));
    s->msi_direct_on = false;
    /* Where MSI-X was already disabled, unset called no mask notifier. */
    WITH_QEMU_LOCK_GUARD(&s->msi_lock) {
        s->msi_unmasked = false;
    }
}

static void reims_graphics_pci_write_config(PCIDevice *dev, uint32_t address,
                                            uint32_t val, int len)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(dev);
    bool was_enabled = msix_enabled(dev);

    pci_default_write_config(dev, address, val, len);
    if (s->irqfd_made) {
        if (!was_enabled && msix_enabled(dev)) {
            reims_graphics_pci_irqfd_enable(s);
        } else if (was_enabled && !msix_enabled(dev)) {
            reims_graphics_pci_irqfd_disable(s);
        }
    } else if (s->msi_direct_made) {
        if (!was_enabled && msix_enabled(dev)) {
            reims_graphics_pci_direct_enable(s);
        } else if (was_enabled && !msix_enabled(dev)) {
            reims_graphics_pci_direct_disable(s);
        }
    }
}

/* ------ Input from the displays' windows ------ */

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
    uint32_t head; /* the display whose window or cursor it is */
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

/*
 * The pointer of @con's window, in its display's pixels. The guest has one
 * pointing device, whose buttons every window's pointer shares.
 */
static void reims_graphics_input_pointer(ReimsGraphicsPCIState *s,
                                         QemuConsole *con,
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
    qemu_input_queue_abs(con, INPUT_AXIS_X,
                         MIN(event->pointer.x, width - 1), 0, width);
    qemu_input_queue_abs(con, INPUT_AXIS_Y,
                         MIN(event->pointer.y, height - 1), 0, height);
    qemu_input_update_buttons(con, button_map, s->input_buttons,
                              event->pointer.buttons);
    s->input_buttons = event->pointer.buttons;
    qemu_input_event_sync();
}

/*
 * @notches wheel notches on one axis: a press and release of @up for each
 * notch above 0, of @down for each below.
 */
static void reims_graphics_input_wheel_axis(QemuConsole *con,
                                            int32_t notches, InputButton up,
                                            InputButton down)
{
    InputButton button = notches > 0 ? up : down;
    int64_t count = MIN(ABS((int64_t)notches),
                        REIMS_GRAPHICS_PCI_WHEEL_MAX_NOTCHES);

    for (int64_t i = 0; i < count; i++) {
        qemu_input_queue_btn(con, button, true);
        qemu_input_queue_btn(con, button, false);
        qemu_input_event_sync();
    }
}

/*
 * A scroll event: smooth scrolling where the device that takes the wheel
 * takes it, the event's notches elsewhere, up and left above 0.
 */
static void reims_graphics_input_scroll(QemuConsole *con,
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

    if (!qemu_input_scrolls_smoothly(con)) {
        reims_graphics_input_wheel_axis(con, scroll->notches_dy,
                                        INPUT_BUTTON_WHEEL_UP,
                                        INPUT_BUTTON_WHEEL_DOWN);
        reims_graphics_input_wheel_axis(con, scroll->notches_dx,
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
    qemu_input_queue_scroll(con, INPUT_AXIS_Y, scroll->dy, phase,
                            momentum);
    qemu_input_queue_scroll(con, INPUT_AXIS_X, scroll->dx, phase,
                            momentum);
    qemu_input_event_sync();
}

static void reims_graphics_input_key(QemuConsole *con, uint16_t usage,
                                     bool down)
{
    unsigned int lnx = 0;

    if (usage < qemu_input_map_usb_to_linux_len) {
        lnx = qemu_input_map_usb_to_linux[usage];
    }
    if (lnx != 0) {
        qemu_input_event_send_key_linux(con, lnx, down);
    }
}

/* ------ The guest's cursors, on the consoles ------ */

/*
 * The console's cursor place and whether it shows, of @head's display. The
 * library gives the place of the hot spot; QEMU's displays take the image's
 * top-left corner, as virtio-gpu gives it, and SPICE adds the hot spot back.
 */
static void reims_graphics_cursor_update(ReimsGraphicsHead *head)
{
    ReimsGraphicsPCIState *s = head->s;
    RvgDisplayCoord at;
    int hot_x = 0;
    int hot_y = 0;

    assert(bql_locked());
    if (!head->con || !s->device) {
        return;
    }
    at = rvg_graphics_device_cursor_position_at(s->device, head->index);
    if (head->cursor) {
        hot_x = head->cursor->hot_x;
        hot_y = head->cursor->hot_y;
    }
    qemu_console_set_mouse(head->con, at.x - hot_x, at.y - hot_y,
                           head->cursor_show && head->cursor);
}

static void reims_graphics_cursor_glyph(ReimsGraphicsHead *head,
                                        ReimsGraphicsEvent *event)
{
    const uint8_t *px = event->glyph.rgba;
    size_t count = (size_t)event->glyph.width * event->glyph.height;
    QEMUCursor *cursor;

    if (!head->con) {
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
    cursor_unref(head->cursor);
    head->cursor = cursor;
    qemu_console_set_cursor(head->con, cursor);
    reims_graphics_cursor_update(head);
}

static void reims_graphics_event_bh(void *opaque)
{
    ReimsGraphicsPCIState *s = opaque;
    ReimsGraphicsEvent *event;
    ReimsGraphicsHead *head;

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
        /*
         * The teardown closed the consoles: an event a callback queued past it
         * (reims_graphics_pci_teardown) goes nowhere.
         */
        head = &s->heads[event->head];
        if (!head->con) {
            reims_graphics_event_free(event);
            continue;
        }
        switch (event->kind) {
        case REIMS_GRAPHICS_EVENT_POINTER:
            reims_graphics_input_pointer(s, head->con, event);
            break;
        case REIMS_GRAPHICS_EVENT_SCROLL:
            reims_graphics_input_scroll(head->con, &event->scroll);
            break;
        case REIMS_GRAPHICS_EVENT_KEY:
            reims_graphics_input_key(head->con, event->key.usage,
                                     event->key.down);
            break;
        case REIMS_GRAPHICS_EVENT_CLOSE:
            /* As QEMU's own windows do: closing one powers the VM off. */
            shutdown_action = SHUTDOWN_ACTION_POWEROFF;
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            break;
        case REIMS_GRAPHICS_EVENT_CURSOR_GLYPH:
            reims_graphics_cursor_glyph(head, event);
            break;
        case REIMS_GRAPHICS_EVENT_CURSOR_SHOW:
            head->cursor_show = event->show;
            reims_graphics_cursor_update(head);
            break;
        case REIMS_GRAPHICS_EVENT_CURSOR_MOVE:
            reims_graphics_cursor_update(head);
            break;
        default:
            g_assert_not_reached();
        }
        reims_graphics_event_free(event);
    }
}

/*
 * Queues @event behind every earlier one of every display, on the windows'
 * thread (on macOS the process's main thread, which QEMU leaves in its run
 * loop under -display none, and on Linux the library's reims-vgpu.ui thread)
 * or on the library's display thread, for the cursors. None of them holds the
 * BQL, and the windows' may not block, so the BH runs the input and the
 * cursors.
 */
static void reims_graphics_event_push(ReimsGraphicsPCIState *s,
                                      ReimsGraphicsEvent *event)
{
    WITH_QEMU_LOCK_GUARD(&s->event_lock) {
        QSIMPLEQ_INSERT_TAIL(&s->events, event, next);
    }
    qemu_bh_schedule(s->event_bh);
}

static ReimsGraphicsEvent *reims_graphics_event_new(ReimsGraphicsEventKind kind,
                                                    uint32_t head)
{
    ReimsGraphicsEvent *event = g_new0(ReimsGraphicsEvent, 1);

    event->kind = kind;
    event->head = head;
    return event;
}

/* A window's callbacks, on the windows' thread: @ctx is its display's head. */
static void reims_graphics_window_pointer_cb(void *ctx, uint16_t x, uint16_t y,
                                             uint16_t width, uint16_t height,
                                             uint32_t buttons)
{
    ReimsGraphicsHead *head = ctx;
    ReimsGraphicsEvent *event =
        reims_graphics_event_new(REIMS_GRAPHICS_EVENT_POINTER, head->index);

    event->pointer.x = x;
    event->pointer.y = y;
    event->pointer.width = width;
    event->pointer.height = height;
    event->pointer.buttons = buttons;
    reims_graphics_event_push(head->s, event);
}

static void reims_graphics_window_scroll_cb(void *ctx,
                                            const RvgWindowScroll *scroll)
{
    ReimsGraphicsHead *head = ctx;
    ReimsGraphicsEvent *event =
        reims_graphics_event_new(REIMS_GRAPHICS_EVENT_SCROLL, head->index);

    /* The fields the library's version has; the rest stay 0. */
    memcpy(&event->scroll, scroll,
           MIN(scroll->struct_size, sizeof(event->scroll)));
    reims_graphics_event_push(head->s, event);
}

/* usage is a USB HID keyboard usage (page 0x07). */
static void reims_graphics_window_key_cb(void *ctx, uint16_t usage, bool down)
{
    ReimsGraphicsHead *head = ctx;
    ReimsGraphicsEvent *event =
        reims_graphics_event_new(REIMS_GRAPHICS_EVENT_KEY, head->index);

    event->key.usage = usage;
    event->key.down = down;
    reims_graphics_event_push(head->s, event);
}

static void reims_graphics_window_close_cb(void *ctx)
{
    ReimsGraphicsHead *head = ctx;
    ReimsGraphicsEvent *event =
        reims_graphics_event_new(REIMS_GRAPHICS_EVENT_CLOSE, head->index);

    reims_graphics_event_push(head->s, event);
}

/*
 * The cursors' callbacks, on the library's display thread: display 0's
 * (cursor_glyph, cursor_show, cursor_move) and, naming it, each other
 * display's (the _at three), for a display the device has.
 */
static void reims_graphics_cursor_glyph_at_cb(void *ctx, uint32_t display,
                                              const RvgCursorGlyph *glyph)
{
    ReimsGraphicsPCIState *s = ctx;
    ReimsGraphicsEvent *event;
    /* height rows of width * 4 bytes */
    size_t length = (size_t)glyph->width * 4 * glyph->height;

    if (display >= s->displays) {
        return;
    }
    event = reims_graphics_event_new(REIMS_GRAPHICS_EVENT_CURSOR_GLYPH,
                                     display);
    event->glyph.width = glyph->width;
    event->glyph.height = glyph->height;
    event->glyph.hot_spot = glyph->hot_spot;
    event->glyph.rgba = g_malloc0(length);
    memcpy(event->glyph.rgba, glyph->rgba, MIN(glyph->rgba_length, length));
    reims_graphics_event_push(s, event);
}

static void reims_graphics_cursor_glyph_cb(void *ctx,
                                           const RvgCursorGlyph *glyph)
{
    reims_graphics_cursor_glyph_at_cb(ctx, 0, glyph);
}

/* show is the library's byte: any value but 0 shows the cursor. */
static void reims_graphics_cursor_show_at_cb(void *ctx, uint32_t display,
                                             uint8_t show)
{
    ReimsGraphicsPCIState *s = ctx;
    ReimsGraphicsEvent *event;

    if (display >= s->displays) {
        return;
    }
    event = reims_graphics_event_new(REIMS_GRAPHICS_EVENT_CURSOR_SHOW,
                                     display);
    event->show = show != 0;
    reims_graphics_event_push(s, event);
}

static void reims_graphics_cursor_show_cb(void *ctx, uint8_t show)
{
    reims_graphics_cursor_show_at_cb(ctx, 0, show);
}

static void reims_graphics_cursor_move_at_cb(void *ctx, uint32_t display)
{
    ReimsGraphicsPCIState *s = ctx;

    if (display >= s->displays) {
        return;
    }
    reims_graphics_event_push(
        s, reims_graphics_event_new(REIMS_GRAPHICS_EVENT_CURSOR_MOVE,
                                    display));
}

static void reims_graphics_cursor_move_cb(void *ctx)
{
    reims_graphics_cursor_move_at_cb(ctx, 0);
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
 * library opens a window for each display, unless REIMS_VGPU_WINDOW is "0".
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

/*
 * The doorbell pages: no register to read, and a write to the library on the
 * vCPU, as a DOORBELL write is.
 */
static uint64_t reims_graphics_doorbells_read(void *opaque, hwaddr offset,
                                              unsigned size)
{
    return 0;
}

static void reims_graphics_doorbells_write(void *opaque, hwaddr offset,
                                           uint64_t value, unsigned size)
{
    ReimsGraphicsPCIState *s = opaque;

    rvg_graphics_device_doorbell_write(s->device, offset, value);
}

static const MemoryRegionOps reims_graphics_doorbells_ops = {
    .read = reims_graphics_doorbells_read,
    .write = reims_graphics_doorbells_write,
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

/* ------ The queues' doorbells ------ */

/*
 * Drops route `r`: its ioeventfd goes, in a transaction of its own, before its
 * file is closed, as KVM finds the ioeventfd it removes by the file.
 */
static void reims_graphics_route_free(ReimsGraphicsPCIState *s,
                                      ReimsGraphicsRoute *r)
{
    memory_region_del_eventfd(r->mr, r->addr, 4, true, r->ring_id, &r->bell);
#ifdef CONFIG_POSIX
    close(event_notifier_get_fd(&r->bell));
#else
    event_notifier_cleanup(&r->bell);
#endif
    g_free(r);
}

#ifdef CONFIG_EVENTFD
/*
 * The routes need eventfds (KVM's ioeventfds take one per route): the library
 * offers routed doorbells only to a Linux host that gives route_doorbell, so a
 * build without them (macOS: HVF or TCG) gives none.
 */
static gboolean reims_graphics_route_holds_ring(gpointer key, gpointer value,
                                                gpointer opaque)
{
    ReimsGraphicsRoute *r = value;
    ReimsGraphicsRoute *at = opaque;

    return r->ring_id == at->ring_id && r->mr == at->mr && r->addr == at->addr;
}

/*
 * Takes route `route` of `ring_id` at `addr` in `mr` into `copy`, ours, under
 * the BQL; false, with `copy` closed, once RVG_GRAPHICS_DOORBELL_ROUTES are
 * taken or the teardown dropped them. A route of a ring at the same address
 * another route holds replaces it first: KVM refuses a second ioeventfd of the
 * same value there, and QEMU ends the VM where KVM refuses one. Each change is
 * a transaction of its own, so a replaced ioeventfd is gone before its
 * successor is added.
 */
static bool reims_graphics_route_add(ReimsGraphicsPCIState *s, uint64_t route,
                                     MemoryRegion *mr, hwaddr addr,
                                     uint32_t ring_id, int copy)
{
    ReimsGraphicsRoute at = { .ring_id = ring_id, .mr = mr, .addr = addr };
    ReimsGraphicsRoute *r;

    if (s->routes_closed ||
        g_hash_table_size(s->routes) >= RVG_GRAPHICS_DOORBELL_ROUTES) {
        close(copy);
        return false;
    }
    while ((r = g_hash_table_find(s->routes, reims_graphics_route_holds_ring,
                                  &at)) != NULL) {
        g_hash_table_steal(s->routes, &r->route);
        reims_graphics_route_free(s, r);
    }
    r = g_new0(ReimsGraphicsRoute, 1);
    r->route = route;
    r->ring_id = ring_id;
    r->mr = mr;
    r->addr = addr;
    event_notifier_init_fd(&r->bell, copy);
    memory_region_add_eventfd(mr, addr, 4, true, ring_id, &r->bell);
    g_hash_table_insert(s->routes, &r->route, r);
    return true;
}

/*
 * The library's route_doorbell and route_doorbell_page, at `addr` in `mr`:
 * on its threads as a queue is made or ends,
 * or inside a STATUS write of 0 on the vCPU or our reset. It applies the
 * change before it returns, under the BQL, which it takes where its thread
 * does not hold it: once a route is dropped no write reaches its file, so a
 * queue made again over the same ring gets every write of its own. The BQL's
 * holders wait for none of the library's threads: the teardown at QEMU's quit
 * releases the device with the BQL let go, so a call waiting for it then takes
 * it, finds the routes closed (reims_graphics_routes_close) and returns, and
 * the vCPU holds no lock of ours around the register or page write, both
 * being lockless. Under the BQL the address spaces' flat views it reads are
 * the current ones, held by reference, so a thread of the library's needs no
 * RCU registration for it.
 */
static bool reims_graphics_route_at(ReimsGraphicsPCIState *s, uint64_t route,
                                    MemoryRegion *mr, hwaddr addr,
                                    uint32_t ring_id, int32_t bell)
{
    bool locked = bql_locked();
    bool taken = true;
    ReimsGraphicsRoute *r;
    int copy = -1;

    if (bell >= 0) {
        copy = qemu_dup(bell);
        if (copy < 0) {
            return false;
        }
    }
    if (!locked) {
        bql_lock();
    }
    if (copy >= 0) {
        taken = reims_graphics_route_add(s, route, mr, addr, ring_id, copy);
    } else {
        r = g_hash_table_lookup(s->routes, &route);
        if (r) {
            g_hash_table_steal(s->routes, &r->route);
            reims_graphics_route_free(s, r);
        }
    }
    if (!locked) {
        bql_unlock();
    }
    return taken;
}

static bool reims_graphics_route_doorbell_cb(void *ctx, uint64_t route,
                                             uint32_t ring_id, int32_t bell)
{
    ReimsGraphicsPCIState *s = ctx;

    return reims_graphics_route_at(s, route, &s->registers,
                                   RVG_GRAPHICS_REG_DOORBELL, ring_id, bell);
}

/*
 * The library's route_doorbell_page: as route_doorbell, at the doorbell of
 * page `page` in BAR 4, which only that page's process writes.
 */
static bool reims_graphics_route_doorbell_page_cb(void *ctx, uint64_t route,
                                                  uint32_t page,
                                                  uint32_t ring_id,
                                                  int32_t bell)
{
    ReimsGraphicsPCIState *s = ctx;

    if (page >= RVG_GRAPHICS_DOORBELL_PAGES) {
        return false;
    }
    return reims_graphics_route_at(s, route, &s->doorbells,
                                   (hwaddr)page *
                                   RVG_GRAPHICS_DOORBELL_PAGE_SIZE,
                                   ring_id, bell);
}

#endif /* CONFIG_EVENTFD */

/* The teardown's, under the BQL: every route goes, and none is taken after. */
static void reims_graphics_routes_close(ReimsGraphicsPCIState *s)
{
    GHashTableIter it;
    gpointer value;

    s->routes_closed = true;
    if (s->routes) {
        g_hash_table_iter_init(&it, s->routes);
        while (g_hash_table_iter_next(&it, NULL, &value)) {
            g_hash_table_iter_steal(&it);
            reims_graphics_route_free(s, value);
        }
    }
}

/* ------ The guest's Shared pool ------ */

/*
 * The guest's Shared pool is a range of guest physical addresses with no RAM
 * under it, sized by the library for the host GPU's Shared memory heap and
 * reserved at realize (reims_graphics_pool_room, reims_graphics_pool_reserve),
 * which the library fills with blocks of host memory as the pool grows: one
 * piece a block, each its own RAM region (so the flat view never merges two,
 * and an add or a removal touches no other piece's KVM slot or WHPX mapping),
 * owned by an object of its own, a ReimsGraphicsPoolPiece. The library asks
 * on its threads (pool_add, pool_remove); the asks are queued and carried out
 * on the main loop, under the BQL, in order. Under WHPX the add's hypervisor
 * map runs on the asking thread first, outside the BQL (whpx_map_ahead), and
 * so does a removal's unmap (whpx_unmap_before_del).
 *
 * A piece leaves the guest's address space at its removal, under the BQL, but
 * a flat view of an older generation or a device's DMA map
 * (address_space_map) may still hold it: those hold its owner, so the piece's
 * end, where it tells the library that the memory is free to go
 * (rvg_graphics_device_pool_removed), comes once the last of them goes.
 */

#define TYPE_REIMS_GRAPHICS_POOL_PIECE "reims-graphics-pool-piece"
OBJECT_DECLARE_SIMPLE_TYPE(ReimsGraphicsPoolPiece, REIMS_GRAPHICS_POOL_PIECE)

struct ReimsGraphicsPoolPiece {
    Object parent_obj;
    MemoryRegion mr;
    ReimsGraphicsPCIState *s;
    hwaddr gpa;
};

struct ReimsGraphicsPoolAsk {
    bool add;
    hwaddr gpa;
    uint64_t size;
    void *host;
    bool mapped_ahead; /* whpx_map_ahead took it, or for a removal */
                       /* whpx_unmap_before_del */
    int64_t asked;     /* QEMU_CLOCK_REALTIME ns, at the ask */
    int64_t map_ns;    /* the map or unmap ahead's time */
    QSIMPLEQ_ENTRY(ReimsGraphicsPoolAsk) next;
};

static void reims_graphics_pool_piece_finalize(Object *obj)
{
    ReimsGraphicsPoolPiece *piece = REIMS_GRAPHICS_POOL_PIECE(obj);
    ReimsGraphicsPCIState *s = piece->s;

    /* Its region went before this, with the piece's properties. */
    s->pool_alive--;
    trace_reims_graphics_pool_removed(piece->gpa);
    if (s->device) {
        rvg_graphics_device_pool_removed(s->device, piece->gpa);
    }
}

/* The added piece at @gpa, or -1. */
static int reims_graphics_pool_find(ReimsGraphicsPCIState *s, hwaddr gpa)
{
    guint i;

    for (i = 0; i < s->pool_pieces->len; i++) {
        ReimsGraphicsPoolPiece *p = g_ptr_array_index(s->pool_pieces, i);

        if (p->gpa == gpa) {
            return i;
        }
    }
    return -1;
}

/* Whether @gpa and @size lie in the range and overlap no added piece. */
static bool reims_graphics_pool_free(ReimsGraphicsPCIState *s, hwaddr gpa,
                                     uint64_t size)
{
    guint i;

    if (gpa < s->pool_base || size > s->pool_length ||
        gpa - s->pool_base > s->pool_length - size) {
        return false;
    }
    for (i = 0; i < s->pool_pieces->len; i++) {
        ReimsGraphicsPoolPiece *p = g_ptr_array_index(s->pool_pieces, i);
        uint64_t len = memory_region_size(&p->mr);

        if (gpa < p->gpa + len && p->gpa < gpa + size) {
            return false;
        }
    }
    return true;
}

static void reims_graphics_pool_add(ReimsGraphicsPCIState *s,
                                    ReimsGraphicsPoolAsk *ask)
{
    ReimsGraphicsPoolPiece *piece;
    int64_t t0, t1;

    if (!reims_graphics_pool_free(s, ask->gpa, ask->size)) {
        error_report_once("%s: the library asked a pool piece of 0x%" PRIx64
                          " bytes at 0x%" PRIx64 ", outside the pool's free "
                          "range: not mapped", TYPE_REIMS_GRAPHICS_PCI,
                          ask->size, ask->gpa);
        if (ask->mapped_ahead) {
            whpx_unmap_ahead(ask->gpa, ask->size, ask->host);
        }
        return;
    }
    t0 = get_clock();
    piece = REIMS_GRAPHICS_POOL_PIECE(object_new(TYPE_REIMS_GRAPHICS_POOL_PIECE));
    piece->s = s;
    piece->gpa = ask->gpa;
    memory_region_init_ram_ptr(&piece->mr, OBJECT(piece),
                               "reims-graphics-pci.pool-piece", ask->size,
                               ask->host);
    /*
     * Inside the add's transaction: on its own, set_unmergeable commits
     * (and renders every flat view) for a region that is in none yet.
     */
    memory_region_transaction_begin();
    memory_region_set_unmergeable(&piece->mr, true);
    memory_region_add_subregion(get_system_memory(), ask->gpa, &piece->mr);
    memory_region_transaction_commit();
    t1 = get_clock();
    g_ptr_array_add(s->pool_pieces, piece);
    s->pool_alive++;
    trace_reims_graphics_pool_add(ask->gpa, ask->size, ask->mapped_ahead,
                                  qemu_clock_get_ns(QEMU_CLOCK_REALTIME) -
                                  ask->asked, ask->map_ns, t1 - t0);
    rvg_graphics_device_pool_added(s->device, ask->gpa);
}

/* Takes the piece at index @i out of the guest's address space. */
static void reims_graphics_pool_take(ReimsGraphicsPCIState *s, guint i)
{
    ReimsGraphicsPoolPiece *piece = g_ptr_array_steal_index(s->pool_pieces, i);

    memory_region_del_subregion(get_system_memory(), &piece->mr);
    /* Its end, and the library's word, wait for its last holder. */
    object_unref(OBJECT(piece));
}

static void reims_graphics_pool_remove(ReimsGraphicsPCIState *s,
                                       ReimsGraphicsPoolAsk *ask)
{
    int i = reims_graphics_pool_find(s, ask->gpa);
    int64_t t0;

    if (i < 0) {
        /* Its add was refused: nothing of it is in the guest. */
        rvg_graphics_device_pool_removed(s->device, ask->gpa);
        return;
    }
    t0 = get_clock();
    memory_region_transaction_begin();
    reims_graphics_pool_take(s, i);
    memory_region_transaction_commit();
    trace_reims_graphics_pool_remove(ask->gpa, ask->size, ask->mapped_ahead,
                                     ask->map_ns, get_clock() - t0);
}

/* The library's asks, carried out in order under the BQL. */
static void reims_graphics_pool_bh(void *opaque)
{
    ReimsGraphicsPCIState *s = opaque;
    ReimsGraphicsPoolAsk *ask;

    for (;;) {
        WITH_QEMU_LOCK_GUARD(&s->pool_lock) {
            ask = QSIMPLEQ_FIRST(&s->pool_asks);
            if (ask) {
                QSIMPLEQ_REMOVE_HEAD(&s->pool_asks, next);
            }
        }
        if (!ask) {
            return;
        }
        if (ask->add) {
            reims_graphics_pool_add(s, ask);
        } else {
            reims_graphics_pool_remove(s, ask);
        }
        g_free(ask);
    }
}

/* Queues @ask for the main loop; false where the teardown took the pool. */
static bool reims_graphics_pool_queue(ReimsGraphicsPCIState *s,
                                      ReimsGraphicsPoolAsk *ask)
{
    WITH_QEMU_LOCK_GUARD(&s->pool_lock) {
        if (s->pool_closed) {
            return false;
        }
        QSIMPLEQ_INSERT_TAIL(&s->pool_asks, ask, next);
    }
    qemu_bh_schedule(s->pool_bh);
    return true;
}

/*
 * The library's pool_add, on its pool thread, the BQL not held: under WHPX
 * the hypervisor's map runs here, then the add is queued for the main loop.
 */
static void reims_graphics_pool_add_cb(void *ctx, uint64_t physical_address,
                                       void *host_address, uint64_t length)
{
    ReimsGraphicsPCIState *s = ctx;
    ReimsGraphicsPoolAsk *ask = g_new0(ReimsGraphicsPoolAsk, 1);
    int64_t t0;

    ask->add = true;
    ask->gpa = physical_address;
    ask->size = length;
    ask->host = host_address;
    ask->asked = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (whpx_enabled()) {
        t0 = get_clock();
        ask->mapped_ahead = whpx_map_ahead(host_address, physical_address,
                                           length);
        ask->map_ns = get_clock() - t0;
    }
    if (!reims_graphics_pool_queue(s, ask)) {
        if (ask->mapped_ahead) {
            whpx_unmap_ahead(ask->gpa, ask->size, ask->host);
        }
        g_free(ask);
    }
}

/*
 * The library's pool_remove, on the thread freeing the block, the BQL not
 * held: under WHPX the hypervisor's unmap runs here, then the removal is
 * queued for the main loop. The guest holds no mapping of the piece by now.
 */
static void reims_graphics_pool_remove_cb(void *ctx, uint64_t physical_address,
                                          uint64_t length)
{
    ReimsGraphicsPCIState *s = ctx;
    ReimsGraphicsPoolAsk *ask = g_new0(ReimsGraphicsPoolAsk, 1);
    bool closed = false;
    int64_t t0;

    ask->gpa = physical_address;
    ask->size = length;
    if (whpx_enabled()) {
        WITH_QEMU_LOCK_GUARD(&s->pool_lock) {
            closed = s->pool_closed;
        }
        /* Once closed, the teardown takes the piece out itself. */
        if (!closed) {
            t0 = get_clock();
            ask->mapped_ahead = whpx_unmap_before_del(physical_address, length);
            ask->map_ns = get_clock() - t0;
        }
    }
    if (!reims_graphics_pool_queue(s, ask)) {
        /* The teardown takes the piece, and its end says so. */
        g_free(ask);
    }
}

/*
 * The teardown's part, under the BQL, before the device's release: no ask is
 * carried out from here on, every piece leaves the guest in one transaction,
 * and the pieces' ends, which tell the library, run before the release where
 * nothing else holds them (drain_call_rcu lets the BQL go meanwhile). The
 * library frees the pieces' memory at the release, after this.
 */
static void reims_graphics_pool_close(ReimsGraphicsPCIState *s)
{
    ReimsGraphicsPoolAsk *ask;

    if (!s->pool_bh) {
        return;
    }
    WITH_QEMU_LOCK_GUARD(&s->pool_lock) {
        s->pool_closed = true;
        while ((ask = QSIMPLEQ_FIRST(&s->pool_asks)) != NULL) {
            QSIMPLEQ_REMOVE_HEAD(&s->pool_asks, next);
            if (ask->mapped_ahead) {
                whpx_unmap_ahead(ask->gpa, ask->size, ask->host);
            }
            g_free(ask);
        }
    }
    if (s->pool_pieces->len > 0) {
        memory_region_transaction_begin();
        while (s->pool_pieces->len > 0) {
            reims_graphics_pool_take(s, s->pool_pieces->len - 1);
        }
        memory_region_transaction_commit();
    }
    if (s->pool_alive > 0) {
        drain_call_rcu();
    }
    if (s->pool_alive > 0) {
        warn_report("%s: %d pool pieces are still held past their removal: "
                    "their memory goes at the release all the same",
                    TYPE_REIMS_GRAPHICS_PCI, s->pool_alive);
    }
}

/*
 * The guest physical addresses the machine can give the pool, *@room bytes
 * from *@base: on a PC machine (one whose PCI host bridge answers
 * pci-hole64-end, q35's and i440fx's), from the first 1 GiB past the RAM and
 * the bridge's default 64-bit window, which ends 32 GiB past the RAM before
 * any BAR is placed, to the top quarter of the guest's physical addresses,
 * where OVMF puts its own 64-bit window when it sizes it from them, in whole
 * GiB. The library takes as much of it as the host GPU's Shared memory heap
 * holds (rvg_graphics_device_pool_range), and reims_graphics_pool_reserve
 * then reserves that. False, with no pool, on any other machine or where
 * nothing fits.
 */
static bool reims_graphics_pool_room(PCIDevice *dev, hwaddr *base,
                                     uint64_t *room)
{
    Object *host = OBJECT(BUS(pci_device_root_bus(dev))->parent);
    uint64_t start, limit, bits = 0;

    if (target_aarch64() || !fw_cfg_find() ||
        !object_property_find(host, "pci-hole64-end")) {
        return false;
    }
    start = ROUND_UP(object_property_get_uint(host, "pci-hole64-end", NULL),
                     GiB);
    if (first_cpu && object_property_find(OBJECT(first_cpu), "phys-bits")) {
        bits = object_property_get_uint(OBJECT(first_cpu), "phys-bits", NULL);
    }
    limit = bits >= 32 && bits < 64 ? (1ULL << bits) / 4 * 3 : UINT64_MAX;
    if (start >= limit) {
        warn_report("%s: the guest's %" PRIu64 " physical address bits leave "
                    "no room for its pool past 0x%" PRIx64,
                    TYPE_REIMS_GRAPHICS_PCI, bits, start);
        return false;
    }
    *base = start;
    *room = QEMU_ALIGN_DOWN(limit - start, GiB);
    return *room != 0;
}

/*
 * Reserves the range the library took of the pool's room
 * (rvg_graphics_device_pool_range), once the device is made: the firmware gets
 * etc/reserved-memory-end past it, rounded up to 1 GiB within the room, so
 * OVMF puts its 64-bit window above it otherwise, as above memory hotplug's.
 * Nothing lies under the range: a piece's add is a pure add. No pool where the
 * library took none (the host GPU has no Shared memory).
 */
static void reims_graphics_pool_reserve(ReimsGraphicsPCIState *s)
{
    FWCfgState *fw_cfg = fw_cfg_find();
    uint64_t length = 0, end, *val, *old;
    hwaddr base;

    base = rvg_graphics_device_pool_range(s->device, &length);
    if (!length || !fw_cfg) {
        return;
    }
    end = ROUND_UP(base + length, GiB);
    val = g_new(uint64_t, 1);
    *val = cpu_to_le64(end);
    old = fw_cfg_modify_file(fw_cfg, "etc/reserved-memory-end", val,
                             sizeof(*val));
    if (old && le64_to_cpu(*old) > end) {
        /* Memory hotplug's area ends past the pool: its end stays. */
        *val = *old;
    }
    g_free(old);
    s->pool_base = base;
    s->pool_length = length;
    trace_reims_graphics_pool_reserved(base, length, end);

#ifdef CONFIG_LINUX
    /*
     * A piece's removal deletes its KVM slot: by default that zaps every
     * slot's second-level maps, which the guest's RAM then refaults (1 GiB of
     * 4 KiB maps in about 0.3 s, measured 11-17). With the quirk off it zaps
     * only the deleted slot's.
     */
    if (!reims_graphics_kvm_disable_slot_zap_all()) {
        warn_report("%s: KVM kept zapping every slot at a slot's delete: a "
                    "pool piece's removal refaults the guest's RAM",
                    TYPE_REIMS_GRAPHICS_PCI);
    }
#endif
}

/* The x-pool-grow property: asks the library for that many more bytes. */
static void reims_graphics_pool_grow_set(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(obj);
    uint64_t value;
    RvgStatus status;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (!s->device) {
        error_setg(errp, "%s: the device is not realized",
                   TYPE_REIMS_GRAPHICS_PCI);
        return;
    }
    status = rvg_graphics_device_pool_grow(s->device, value);
    if (status != RVG_OK) {
        error_setg(errp, "%s: rvg_graphics_device_pool_grow: %s",
                   TYPE_REIMS_GRAPHICS_PCI, rvg_status_message(status));
    }
}

/* ------ Initialisation and startup ------ */

/* ------ The consoles: what each display shows, read back ------ */

typedef struct ReimsGraphicsReadJob {
    ReimsGraphicsHead *head;
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
    ReimsGraphicsHead *head = job->head;
    ReimsGraphicsPCIState *s = head->s;

    /*
     * This runs on a pool thread, not under the BQL: the read waits for its
     * draw on the GPU. The library gives the mode's size whatever it answers,
     * so a buffer too small for the mode is made its size, and the read runs
     * once more.
     */
    job->status = rvg_graphics_device_read_display_at(job->device, head->index,
                                                      job->pixels, job->length,
                                                      &job->width,
                                                      &job->height);
    if (job->status == RVG_ERR_INVALID_ARGUMENT &&
        (size_t)job->width * job->height * 4 > job->length) {
        job->length = (size_t)job->width * job->height * 4;
        job->pixels = g_realloc(job->pixels, job->length);
        job->status = rvg_graphics_device_read_display_at(job->device,
                                                          head->index,
                                                          job->pixels,
                                                          job->length,
                                                          &job->width,
                                                          &job->height);
    }
    WITH_QEMU_LOCK_GUARD(&s->read_lock) {
        head->read_running = false;
        qemu_cond_broadcast(&s->read_ended);
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            reims_graphics_pci_read_done_bh, job);
    return 0;
}

static void reims_graphics_pci_read_done_bh(void *opaque)
{
    ReimsGraphicsReadJob *job = opaque;
    ReimsGraphicsHead *head = job->head;
    DisplaySurface *surface = head->surface;
    size_t stride = (size_t)job->width * 4;
    uint32_t y;

    assert(bql_locked());
    head->reading = false;
    head->read_late = false;
    if (head->read_bound) {
        timer_del(head->read_bound);
    }
    if (job->status != RVG_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: rvg_graphics_device_read_display_at %u: %s\n",
                      TYPE_REIMS_GRAPHICS_PCI, head->index,
                      rvg_status_message(job->status));
    } else if (head->con) {
        if (!surface || surface_width(surface) != job->width ||
            surface_height(surface) != job->height) {
            /* The console frees the surface it had. */
            surface = qemu_create_displaysurface(job->width, job->height);
            head->surface = surface;
            qemu_console_set_surface(head->con, surface);
        }
        /* The pixels have the surface's size and pixman's x8r8g8b8 layout. */
        for (y = 0; y < job->height; y++) {
            memcpy((uint8_t *)surface_data(surface) +
                   (size_t)y * surface_stride(surface),
                   job->pixels + y * stride, stride);
        }
        qemu_console_update_full(head->con);
    }
    g_free(head->pixels);
    head->pixels = job->pixels;
    head->pixels_length = job->length;
    g_free(job);
    /*
     * The update gfx_update asked for, if one did, read or not: an update
     * that waits for it, a screendump's, never waits for another.
     */
    qemu_console_hw_update_done(head->con);
}

/*
 * A read of a display REIMS_GRAPHICS_PCI_READ_LATE_MS after its start,
 * still waiting for its draw on the GPU, which a GPU that stopped answering
 * never finishes: the read is late. An update waiting for it, a screendump's,
 * is answered now with what its console shows: the frame of the last read
 * that ended, or QEMU's placeholder where none has. So is every update of
 * the console until the read ends, which starts no other read of the display
 * meanwhile: QMP runs one command at
 * a time, and a screendump's holds every monitor until it is answered. The
 * read's end updates the console as any read's does.
 */
static void reims_graphics_pci_read_bound(void *opaque)
{
    ReimsGraphicsHead *head = opaque;

    assert(bql_locked());
    if (!head->reading) {
        return;
    }
    head->read_late = true;
    warn_report("%s: a read of display %u still waits for its draw on the "
                "GPU after %d ms: its console shows the last frame read until "
                "it ends", TYPE_REIMS_GRAPHICS_PCI, head->index,
                REIMS_GRAPHICS_PCI_READ_LATE_MS);
    qemu_console_hw_update_done(head->con);
}

/*
 * QEMU's display system asks for a console's surface: a display change
 * listener's refresh, or a screendump of its head. A read of the head's
 * display starts, unless one is under way, and signals the update done; it
 * reads what the display shows when it runs. An update while a read is under
 * way waits for it, but for REIMS_GRAPHICS_PCI_READ_LATE_MS from its start at
 * most: once it is late, an update is done at once
 * (reims_graphics_pci_read_bound). Each display's reads are its own.
 */
static bool reims_graphics_pci_gfx_update(void *opaque)
{
    ReimsGraphicsHead *head = opaque;
    ReimsGraphicsPCIState *s = head->s;
    ReimsGraphicsReadJob *job;

    assert(bql_locked());
    if (head->reading) {
        return head->read_late;
    }
    if (!s->device || !s->pool) {
        return true;
    }
    head->reading = true;
    timer_mod(head->read_bound, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                                REIMS_GRAPHICS_PCI_READ_LATE_MS);
    job = g_new0(ReimsGraphicsReadJob, 1);
    job->head = head;
    job->device = s->device;
    job->pixels = head->pixels;
    job->length = head->pixels_length;
    head->pixels = NULL;
    head->pixels_length = 0;
    WITH_QEMU_LOCK_GUARD(&s->read_lock) {
        head->read_running = true;
    }
    thread_pool_submit_immediate(s->pool, reims_graphics_pci_do_read, job,
                                 NULL);
    return false;
}

/* Whether a read of any display is in the library (s->read_lock held). */
static bool reims_graphics_pci_read_running(ReimsGraphicsPCIState *s)
{
    uint32_t i;

    for (i = 0; i < RVG_GRAPHICS_DISPLAYS_MAX; i++) {
        if (s->heads[i].read_running) {
            return true;
        }
    }
    return false;
}

/*
 * Whether no read of any display is in the library, waiting for those under
 * way at most REIMS_GRAPHICS_PCI_READ_END_MS: a read waits for its draw on
 * the GPU, which a GPU that stopped answering never finishes.
 */
static bool reims_graphics_pci_read_ended(ReimsGraphicsPCIState *s)
{
    int64_t deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                       REIMS_GRAPHICS_PCI_READ_END_MS;
    int64_t left;

    QEMU_LOCK_GUARD(&s->read_lock);
    while (reims_graphics_pci_read_running(s)) {
        left = deadline - qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        if (left <= 0) {
            return false;
        }
        qemu_cond_timedwait(&s->read_ended, &s->read_lock, left);
    }
    return true;
}

static const GraphicHwOps reims_graphics_pci_console_ops = {
    .gfx_update = reims_graphics_pci_gfx_update,
};

static void reims_graphics_pci_init(Object *obj)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(obj);
    uint32_t i;

    memory_region_init(&s->bar0, obj, "reims-graphics-pci.bar0",
                       RVG_GRAPHICS_REGISTERS_SIZE);
    memory_region_init_io(&s->registers, obj, &reims_graphics_pci_ops, s,
                          "reims-graphics-pci.registers",
                          RVG_GRAPHICS_MSIX_OFFSET);
    memory_region_enable_lockless_io(&s->registers);
    memory_region_add_subregion(&s->bar0, 0, &s->registers);
    memory_region_init_io(&s->doorbells, obj, &reims_graphics_doorbells_ops, s,
                          "reims-graphics-pci.doorbells",
                          RVG_GRAPHICS_DOORBELLS_SIZE);
    memory_region_enable_lockless_io(&s->doorbells);
    qemu_mutex_init(&s->event_lock);
    QSIMPLEQ_INIT(&s->events);
    s->routes = g_hash_table_new(g_int64_hash, g_int64_equal);
    qemu_mutex_init(&s->read_lock);
    qemu_cond_init(&s->read_ended);
    qemu_mutex_init(&s->pool_lock);
    QSIMPLEQ_INIT(&s->pool_asks);
    s->pool_pieces = g_ptr_array_new();
    for (i = 0; i < RVG_GRAPHICS_DISPLAYS_MAX; i++) {
        s->heads[i].s = s;
        s->heads[i].index = i;
    }
}

/*
 * Undoes what realize did, from its failure, at unrealize, or at QEMU's quit
 * (reims_graphics_pci_vm_state_change), with BAR 2 out of the guest's address
 * space; a second call finds nothing left to undo but what a read kept. The
 * reads of the displays under way end first, as they use the device: the
 * teardown waits for them at most REIMS_GRAPHICS_PCI_READ_END_MS, under the
 * BQL, and a read that waits for a GPU which stopped answering ends no
 * sooner. Past that the read keeps the device, its pool and the rest to the
 * process's exit, as a VM whose shutdown pauses it does, and the teardown says
 * so and returns; a second call waits for the reads again. The release
 * of the library's last device waits, at most 5 s, for its threads and the
 * windows' GPU work; its region stays mapped until they let it go. Of our
 * callbacks only route_doorbell takes the BQL, so at QEMU's quit (@quit) the
 * release, and nothing else, runs with the BQL let go: a route_doorbell
 * waiting for it takes it, finds the routes closed and returns, holding the
 * release up no more. Every other callback runs as it would with the BQL
 * held, as none takes it, and no BH of ours runs inside the release, which
 * runs on the main loop's thread. Elsewhere no route_doorbell can wait: a
 * realize that fails ran no guest, and the device is not hotpluggable, so no
 * unrealize comes while its guest runs. A callback can still run past the
 * release: a window's pointer, scroll, key or close already running on the
 * windows' thread as the release closes the windows, which waits for nothing,
 * and a cursor callback or raise_interrupt of a device thread the release's
 * 5 s left running. Such a callback still queues its event and schedules
 * event_bh, which stays to the exit for it and drops what it finds once the
 * consoles are closed, or writes the irqfd, which stays open for it, or
 * schedules a BH of its own: the device is not hotpluggable, so that is only
 * at QEMU's end.
 */
static void reims_graphics_pci_teardown(ReimsGraphicsPCIState *s, bool quit)
{
    PCIDevice *dev = PCI_DEVICE(s);
    RvgGraphicsDevice *device;
    ReimsGraphicsEvent *event;
    ReimsGraphicsHead *head;
    uint32_t i;

    if (s->vm_state) {
        qemu_del_vm_change_state_handler(s->vm_state);
        s->vm_state = NULL;
    }
    for (i = 0; i < RVG_GRAPHICS_DISPLAYS_MAX; i++) {
        head = &s->heads[i];
        if (head->con) {
            /*
             * A screendump waiting for the read under way is answered now,
             * with what the console shows: the read's end updates no closed
             * console, and QEMU's quit waits for the QMP dispatcher the
             * screendump holds (monitor_cleanup).
             */
            qemu_console_hw_update_done(head->con);
            qemu_graphic_console_close(head->con);
            head->con = NULL;
            head->surface = NULL;
        }
        /* The bound serves the console's updates, which have ended. */
        timer_free(head->read_bound);
        head->read_bound = NULL;
    }
    /* The reads under way end first: they use the device. */
    if (s->pool) {
        if (!reims_graphics_pci_read_ended(s)) {
            warn_report("%s: a read of a display still waits for its draw "
                        "on the GPU after %d ms: the device stays to the exit",
                        TYPE_REIMS_GRAPHICS_PCI,
                        REIMS_GRAPHICS_PCI_READ_END_MS);
            return;
        }
        thread_pool_free(s->pool);
        s->pool = NULL;
    }
    for (i = 0; i < RVG_GRAPHICS_DISPLAYS_MAX; i++) {
        head = &s->heads[i];
        g_free(head->pixels);
        head->pixels = NULL;
        head->pixels_length = 0;
    }
    reims_graphics_routes_close(s);
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
    if (s->msi_direct_made) {
        reims_graphics_pci_direct_disable(s);
    }
    if (msix_present(dev)) {
        msix_uninit(dev, &s->bar0, &s->bar0);
    }
    reims_graphics_pool_close(s);
    device = s->device;
    s->device = NULL;
    if (quit) {
        bql_unlock();
        rvg_graphics_device_release(device);
        bql_lock();
    } else {
        rvg_graphics_device_release(device);
    }
    WITH_QEMU_LOCK_GUARD(&s->event_lock) {
        while ((event = QSIMPLEQ_FIRST(&s->events)) != NULL) {
            QSIMPLEQ_REMOVE_HEAD(&s->events, next);
            reims_graphics_event_free(event);
        }
    }
    for (i = 0; i < RVG_GRAPHICS_DISPLAYS_MAX; i++) {
        cursor_unref(s->heads[i].cursor);
        s->heads[i].cursor = NULL;
    }
}

typedef struct ReimsGraphicsMemoryWalk {
    GArray *ranges;
    GArray *files;
} ReimsGraphicsMemoryWalk;

/*
 * One range of the guest's RAM and the file behind it: on Linux a shared
 * mapping of a file in pages of the host's size, which the library's views of
 * an IOSurface's pages map again. Only such ranges go into the map there; RAM
 * with no file (anonymous, private or huge pages) is left out, so the library
 * cannot view a surface there in place and copies it instead. A machine whose
 * RAM is all file-backed (q35 with -object memory-backend-memfd,share=on)
 * gives every range; one with small unbacked regions (vmapple's 1 MiB firmware
 * and 64 KiB config, which hold no IOSurface) gives only its main RAM.
 *
 * On macOS the library views the process's own mapping of the RAM again
 * (mach_vm_remap), with no file: every range of RAM in pages of the host's
 * size goes into the map, with an fd of -1, whatever backs it. A view so made
 * shares the pages of RAM that HVF maps into the guest, both ways.
 *
 * On Windows the library maps views of the section behind a range
 * (MapViewOfFile3), which share the pages WHPX maps into the guest, both ways:
 * only RAM that is a view of a section (memory-backend-ram,share=on) goes
 * into the map, with the section's handle and the range's offset in it, and
 * other RAM is left out, as on Linux.
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
#ifdef _WIN32
    RvgMemoryRangeFile file = { .handle = NULL };
    HANDLE section;
#else
    RvgMemoryRangeFile file = { .fd = -1 };
    int fd;
#endif

    if (!memory_access_is_direct(mr, true, MEMTXATTRS_UNSPECIFIED)) {
        return false;
    }
    if (qemu_ram_pagesize(rb) != qemu_real_host_page_size()) {
        return false;
    }
#ifdef _WIN32
    section = qemu_ram_get_section(rb);
    if (!section) {
        return false;
    }
#else
    fd = memory_region_get_fd(mr);
#ifndef CONFIG_DARWIN
    if (fd < 0 || !qemu_ram_is_shared(rb)) {
        return false;
    }
#endif
#endif
    /* q35's RAM above 4 GiB is an alias of the block past the part below. */
    host = (uint8_t *)memory_region_get_ram_ptr(mr) + offset_in_region;
    range = (RvgMemoryRange) {
        .physical_address = int128_get64(start),
        .physical_length = int128_get64(len),
        .host_address = host,
    };
#if defined(_WIN32)
    /* The block's view maps its whole section from its start. */
    file.handle = section;
    file.offset = qemu_ram_block_host_offset(rb, host);
#elif !defined(CONFIG_DARWIN)
    file.fd = fd;
    file.offset = qemu_ram_block_host_offset(rb, host) +
                  qemu_ram_get_fd_offset(rb);
#else
    (void)fd;
#endif
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

/*
 * At QEMU's quit, once the main loop ended and vm_shutdown stopped the VM for
 * good, its vCPUs paused, with the BQL held: the device is torn down while
 * QEMU still runs, so that the library's release of its last device, which
 * runs with the BQL let go (reims_graphics_pci_teardown), lets go of the GPU
 * device before the process exits. A runtime device kept to the
 * exit can end it with SIGSEGV inside NVIDIA's driver, whose exit handler
 * tears its SPIR-V compiler down while a worker thread of the driver's still
 * runs in it (reims_vgpu.h, rvg_graphics_device_release). BAR 2 leaves the
 * guest's address space first, since the region goes with the device; QEMU
 * runs no more of the device's MMIO.
 *
 * A shutdown whose action pauses the VM (-action shutdown=pause,
 * -no-shutdown) keeps the device, which the guest uses again after a reset,
 * and then so does its quit, as does a quit of a VM stopped before it: no
 * handler hears it. The device then stays to the exit, as before. So does it
 * when a read of a display under way still waits for the GPU 5 s into the
 * teardown (reims_graphics_pci_teardown).
 */
static void reims_graphics_pci_vm_state_change(void *opaque, bool running,
                                               RunState state)
{
    ReimsGraphicsPCIState *s = opaque;

    if (running || state != RUN_STATE_SHUTDOWN ||
        shutdown_action == SHUTDOWN_ACTION_PAUSE) {
        return;
    }
    memory_region_set_enabled(&s->region, false);
    reims_graphics_pci_teardown(s, true);
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
#ifdef CONFIG_EVENTFD
        .route_doorbell = reims_graphics_route_doorbell_cb,
        .route_doorbell_page = reims_graphics_route_doorbell_page_cb,
#endif
        .cursor_glyph_at = reims_graphics_cursor_glyph_at_cb,
        .cursor_show_at = reims_graphics_cursor_show_at_cb,
        .cursor_move_at = reims_graphics_cursor_move_at_cb,
        .pool_add = reims_graphics_pool_add_cb,
        .pool_remove = reims_graphics_pool_remove_cb,
    };
    /* Each display's window, its callbacks' ctx the display's head. */
    RvgWindowCallbacks window_callbacks[RVG_GRAPHICS_DISPLAYS_MAX];
    RvgWindowConfig window_configs[RVG_GRAPHICS_DISPLAYS_MAX];
    const RvgWindowConfig *display_windows[RVG_GRAPHICS_DISPLAYS_MAX - 1];
    bool window = reims_graphics_window_wanted();
    /*
     * The guest's architecture is this QEMU's target: the device answers an
     * arm64 guest's Metal as an Apple silicon Mac's GPU answers where the two
     * kinds of Mac differ (isFramebufferReadSupported).
     */
    RvgGraphicsDeviceConfig config = {
        .struct_size = sizeof(config),
        .callbacks = &callbacks,
        .guest_arch = target_aarch64() ? RVG_GUEST_ARCH_ARM64
                                       : RVG_GUEST_ARCH_X86_64,
        /* BAR 4 is registered below, its writes sent to the library. */
        .doorbell_pages = 1,
    };
    g_autoptr(GArray) ranges = g_array_new(false, false,
                                           sizeof(RvgMemoryRange));
    g_autoptr(GArray) files = g_array_new(false, false,
                                          sizeof(RvgMemoryRangeFile));
    ReimsGraphicsMemoryWalk walk = {
        .ranges = ranges,
        .files = files,
    };
    RvgMemoryMap memory_map;
    RvgStatus status;
    uint16_t mode_width = 0;
    uint16_t mode_height = 0;
    uint8_t *base;
    uint64_t size;
    hwaddr pool_base = 0;
    uint64_t pool_room = 0;
    uint32_t i;

    if (s->displays < 1 || s->displays > RVG_GRAPHICS_DISPLAYS_MAX) {
        error_setg(errp, "%s: displays must be 1 to %d, not %" PRIu32,
                   TYPE_REIMS_GRAPHICS_PCI, RVG_GRAPHICS_DISPLAYS_MAX,
                   s->displays);
        return;
    }
    if (!reims_graphics_parse_resolution(s->resolution, &mode_width,
                                         &mode_height, errp)) {
        return;
    }
    /*
     * Under -display none, a window per display, each at the resolution
     * property's size: the library opens it at that mode, and ignores the
     * flags.
     */
    for (i = 0; i < s->displays; i++) {
        window_callbacks[i] = (RvgWindowCallbacks) {
            .struct_size = sizeof(window_callbacks[i]),
            .ctx = &s->heads[i],
            .pointer = reims_graphics_window_pointer_cb,
            .key = reims_graphics_window_key_cb,
            .close = reims_graphics_window_close_cb,
            .scroll = reims_graphics_window_scroll_cb,
        };
        window_configs[i] = (RvgWindowConfig) {
            .struct_size = sizeof(window_configs[i]),
            .callbacks = &window_callbacks[i],
            .mode_width = mode_width,
            .mode_height = mode_height,
        };
        if (i > 0) {
            display_windows[i - 1] = &window_configs[i];
        }
    }
    config.display_count = s->displays;
    if (window) {
        config.window = &window_configs[0];
        config.display_windows = s->displays > 1 ? display_windows : NULL;
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
     * place. On Linux the library's views of them need the file behind each
     * range, so only the file-backed ranges go into the map; RAM without a
     * file (not a shared memfd, or huge pages) is left out, and a surface
     * whose pages fall there is copied as every surface is without a map. A
     * surface in a mapped range is used in place, and IOSurfaces live only in
     * the main RAM, which a shared memfd backs, so in practice every surface
     * maps in place. On Windows the section behind a range takes the file's
     * place: RAM that is not a view of a section is left out, and the main
     * RAM is one where the host gives memory-backend-ram,share=on. On macOS
     * every range in pages of the host's size goes in, with no file
     * (reims_graphics_add_memory_range), and the library
     * uses a surface in place only where the guest lays its pages out in
     * whole pages of the host's (16 KiB, an arm64 guest). The library keeps
     * views of the map's pages while the device lives, so they must stay the
     * guest's RAM: no balloon or hot-unplug may discard them.
     */
    flatview_for_each_range(address_space_to_flatview(&address_space_memory),
                            reims_graphics_add_memory_range, &walk);
    if (walk.ranges->len > 0) {
        memory_map = (RvgMemoryMap) {
            .ranges = (const RvgMemoryRange *)walk.ranges->data,
            .range_count = walk.ranges->len,
        };
        config.memory_map = &memory_map;
        config.memory_files = (const RvgMemoryRangeFile *)walk.files->data;
    } else {
        warn_report("%s: no guest RAM is "
#if defined(_WIN32)
                    "a shared section "
#elif !defined(CONFIG_DARWIN)
                    "a shared memfd "
#endif
                    "in pages of the host's size: IOSurfaces are copied "
                    "for the GPU",
                    TYPE_REIMS_GRAPHICS_PCI);
    }

    /*
     * The guest's Shared pool, where the machine has room for it: the library
     * takes as much of the room as the host GPU's Shared heap holds, and the
     * range is reserved once it answers (reims_graphics_pool_reserve).
     */
    if (reims_graphics_pool_room(dev, &pool_base, &pool_room)) {
        config.pool_physical_address = pool_base;
        config.pool_physical_length = pool_room;
    }

    /* The windows' input runs from here on; the callbacks need the BHs. */
    s->event_bh = qemu_bh_new(reims_graphics_event_bh, s);
    s->pool_bh = qemu_bh_new(reims_graphics_pool_bh, s);
    status = rvg_graphics_device_create(&config, &s->device);
    if (status != RVG_OK) {
        error_setg(errp, "%s: rvg_graphics_device_create: %s",
                   TYPE_REIMS_GRAPHICS_PCI, rvg_status_message(status));
        goto fail;
    }
    reims_graphics_pool_reserve(s);

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
    } else if (whpx_enabled() && whpx_irqchip_in_kernel()) {
        qemu_mutex_init(&s->msi_lock);
        s->msi_direct_made = true;
    }
    pci_register_bar(dev, RVG_GRAPHICS_PCI_BAR_REGISTERS,
                     PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    if (event_notifier_init(&s->doorbell, false) < 0) {
        error_setg(errp, "%s: the doorbell's event notifier",
                   TYPE_REIMS_GRAPHICS_PCI);
        goto fail;
    }
    s->doorbell_made = true;
#ifdef _WIN32
    status = rvg_graphics_device_set_doorbell(
        s->device, event_notifier_get_handle(&s->doorbell));
#else
    status = rvg_graphics_device_set_doorbell(
        s->device, event_notifier_get_fd(&s->doorbell));
#endif
    if (status != RVG_OK) {
        error_setg(errp, "%s: rvg_graphics_device_set_doorbell: %s",
                   TYPE_REIMS_GRAPHICS_PCI, rvg_status_message(status));
        goto fail;
    }
    memory_region_add_eventfd(&s->registers, RVG_GRAPHICS_REG_DOORBELL, 4,
                              true, RVG_GRAPHICS_DOORBELL_CONTROL,
                              &s->doorbell);
    s->doorbell_added = true;

    pci_register_bar(dev, RVG_GRAPHICS_PCI_BAR_DOORBELLS,
                     PCI_BASE_ADDRESS_SPACE_MEMORY, &s->doorbells);

    base = rvg_graphics_device_region(s->device, &size);
    memory_region_init_ram_ptr(&s->region, OBJECT(dev),
                               "reims-graphics-pci.region", size, base);
    pci_register_bar(dev, RVG_GRAPHICS_PCI_BAR_REGION,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->region);

    /*
     * Each display's console: the source of its window's input, head i of
     * QEMU's UI, and what the display shows, which its reads run in the pool
     * for.
     */
    s->pool = thread_pool_new();
    for (i = 0; i < s->displays; i++) {
        ReimsGraphicsHead *head = &s->heads[i];

        head->read_bound = timer_new_ms(QEMU_CLOCK_REALTIME,
                                        reims_graphics_pci_read_bound, head);
        head->con = qemu_graphic_console_create(
            DEVICE(dev), i, &reims_graphics_pci_console_ops, head);
    }
    s->vm_state = qemu_add_vm_change_state_handler(
        reims_graphics_pci_vm_state_change, s);
    return;

fail:
    reims_graphics_pci_teardown(s, false);
}

static void reims_graphics_pci_exit(PCIDevice *dev)
{
    reims_graphics_pci_teardown(REIMS_GRAPHICS_PCI(dev), false);
}

static void reims_graphics_pci_reset(Object *obj, ResetType type)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(obj);

    /* The reset disables MSI-X without a config write. */
    if (s->irqfd_made) {
        reims_graphics_pci_irqfd_disable(s);
    }
    if (s->msi_direct_made) {
        reims_graphics_pci_direct_disable(s);
    }
    if (s->device) {
        rvg_graphics_device_reset(s->device);
    }
}

static void reims_graphics_pci_finalize(Object *obj)
{
    ReimsGraphicsPCIState *s = REIMS_GRAPHICS_PCI(obj);
    ReimsGraphicsEvent *event;

    /* The teardown keeps event_bh for callbacks past it: it goes with s. */
    if (s->event_bh) {
        qemu_bh_delete(s->event_bh);
    }
    while ((event = QSIMPLEQ_FIRST(&s->events)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&s->events, next);
        reims_graphics_event_free(event);
    }
    qemu_mutex_destroy(&s->event_lock);
    g_hash_table_destroy(s->routes);
    qemu_mutex_destroy(&s->read_lock);
    qemu_cond_destroy(&s->read_ended);
    if (s->msi_direct_made) {
        qemu_mutex_destroy(&s->msi_lock);
    }
    if (s->pool_bh) {
        qemu_bh_delete(s->pool_bh);
    }
    g_ptr_array_free(s->pool_pieces, true);
    qemu_mutex_destroy(&s->pool_lock);
}

static const Property reims_graphics_pci_properties[] = {
    DEFINE_PROP_STRING("resolution", ReimsGraphicsPCIState, resolution),
    DEFINE_PROP_UINT32("displays", ReimsGraphicsPCIState, displays, 1),
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
    object_class_property_add(klass, "x-pool-grow", "uint64",
                              &uint64_type_info, NULL,
                              reims_graphics_pool_grow_set, NULL, NULL);

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
    {
        .name              = TYPE_REIMS_GRAPHICS_POOL_PIECE,
        .parent            = TYPE_OBJECT,
        .instance_size     = sizeof(ReimsGraphicsPoolPiece),
        .instance_finalize = reims_graphics_pool_piece_finalize,
    },
};
DEFINE_TYPES(reims_graphics_pci_types)
