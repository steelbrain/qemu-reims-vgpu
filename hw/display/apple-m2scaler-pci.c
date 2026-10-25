/*
 * QEMU Apple paravirtualized M2 scaler device, virtio-PCI 106b:1a0b
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A macOS guest's AppleM2ScalerParavirtDriver kext hands its
 * IOSurfaceAccelerator's scaling and conversion to the host through this
 * device, as Virtualization.framework gives it. Its host side is reims-vgpu's
 * scaler device (rvg_scaler_device_*, reims_vgpu.h); this file is its
 * transport, a modern (1.x) virtio device with Apple's IDs, no device features
 * and no device configuration, and one queue:
 *
 *  - Each element is one request: its readable part, at most 64 KiB, is
 *    copied whole into one buffer, which the library checks, answers and
 *    rewrites in place. The
 *    reply is all of it, written into the writable part, with a used length
 *    of its size. A request the library refuses sets DEVICE_NEEDS_RESET, with
 *    no reply, and one it says ends the VM ends the process, as Apple's host
 *    does each.
 *  - The library answers on the device's worker thread, not the main loop: a
 *    transform takes the GPU for as long as it takes, and the VM's main loop
 *    runs meanwhile. One element is with the worker at a time; its answer
 *    goes back on the main loop, and only then is the next popped, so the
 *    elements are answered one by one in the guest's order, as Apple's host
 *    answers them on the device's own queue.
 *  - The guest's memory the requests name (the capabilities a type 1 request
 *    asks for, a transform's side buffers) is reached through the device's
 *    DMA address space, and only where it is RAM, as Apple's host reaches
 *    only the guest's RAM.
 *
 * The IOSurfaces a transform names are resolved by the library, through the
 * process's latest IOSurface device (apple-gfx-iosurface-pci on q35,
 * apple-gfx-mmio on vmapple), as apple-videotoolbox-pci's are.
 *
 * DRIVER_OK makes the library's accelerator; a reset drops it, once the
 * element with the worker is answered, which the reset then discards.
 *
 * The device has no properties of its own: the library transforms on the
 * host's video processing, which it picks itself and logs: on the GPU
 * apple-videotoolbox-pci's backend runs on where it has it, sharing that
 * backend, else on another of the host's GPUs that has it. It realizes on
 * every host. Where the host has none (rvg_scaler_device_scales), the device
 * refuses the guest's features, leaving FEATURES_OK unset: the guest's driver
 * reads back the status bits it sets, so its start fails before DRIVER_OK,
 * and the guest does without the device, as in a VM without it.
 *
 * Not for upstream: written with an AI assistant, which QEMU's policy on
 * AI-generated content (docs/devel/code-provenance.rst) declines.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "qemu/thread.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "migration/vmstate.h"
#include "system/memory.h"
#include "system/memory_cached.h"
#include "standard-headers/linux/virtio_ids.h"
#include "trace.h"
#include <reims_vgpu.h>

#define TYPE_APPLE_M2SCALER "apple-m2scaler"
OBJECT_DECLARE_SIMPLE_TYPE(AppleM2Scaler, APPLE_M2SCALER)

#define TYPE_APPLE_M2SCALER_PCI "apple-m2scaler-pci"
OBJECT_DECLARE_SIMPLE_TYPE(AppleM2ScalerPCI, APPLE_M2SCALER_PCI)

/* The queue's size: ours, as Apple's is not read. */
#define APPLE_M2SCALER_QUEUE_SIZE 256

/*
 * The largest readable part the library takes, reims-vgpu-video-core's
 * scaler::MAX_REQUEST_SIZE: ours. A larger one is a needs-reset, checked here
 * before the element is copied, so that a guest cannot have QEMU allocate and
 * copy what its descriptors add up to (Apple's host copies any size). The
 * guest kext's largest request is 0x608 bytes.
 */
#define APPLE_M2SCALER_MAX_REQUEST 0x10000

/*
 * One element with the worker: its readable part, copied, which the library
 * rewrites into the reply, and the library's answer.
 */
typedef struct AppleM2ScalerJob {
    VirtQueueElement *elem;
    uint8_t *request;
    size_t length;
    size_t room;
    RvgScalerOutcome outcome;
    char reason[128];
} AppleM2ScalerJob;

struct AppleM2Scaler {
    VirtIODevice parent_obj;

    VirtQueue *requests;
    RvgScalerDevice *device;
    /* Sets DEVICE_NEEDS_RESET after a DRIVER_OK that made no accelerator. */
    QEMUBH *start_failed_bh;

    /*
     * The worker, which calls rvg_scaler_device_process. Under lock: job, the
     * element it has or is to take (NULL once it answered), done, its answer
     * until answered_bh takes it, and ending. queued is the main loop's alone:
     * an element is with the worker or answered and not yet returned, so no
     * other is popped.
     */
    QemuThread worker;
    QemuMutex lock;
    QemuCond wake;
    QemuCond answered;
    AppleM2ScalerJob *job;
    AppleM2ScalerJob *done;
    bool ending;
    bool queued;
    /* Returns the worker's answer to the guest, on the main loop. */
    QEMUBH *answered_bh;
};

struct AppleM2ScalerPCI {
    VirtIOPCIProxy parent_obj;

    AppleM2Scaler vdev;
};

/*
 * The pieces of [address, address + length) of the device's DMA address
 * space, each mapped by a MemoryRegionCache: ours. A range takes one piece a
 * memory region; the guest's requests name a few kilobytes, so more than
 * eight pieces is refused, as is a range that wraps.
 */
#define APPLE_M2SCALER_MAX_PIECES 8

typedef struct AppleM2ScalerRange {
    MemoryRegionCache pieces[APPLE_M2SCALER_MAX_PIECES];
    hwaddr lengths[APPLE_M2SCALER_MAX_PIECES];
    unsigned count;
} AppleM2ScalerRange;

static void apple_m2scaler_unmap(AppleM2ScalerRange *r)
{
    while (r->count) {
        address_space_cache_destroy(&r->pieces[--r->count]);
    }
}

/*
 * Maps all of [address, address + length) (length 0: the address alone),
 * every piece the guest's RAM and not a RAM device, or none of it. On the
 * worker, without the BQL: each piece holds its memory region, so what is
 * checked here is what is copied, and no access goes to a region whose
 * dispatch would take the BQL, which the main loop may hold waiting for the
 * worker (apple_m2scaler_drain). The DMA address space is taken as it maps:
 * behind an IOMMU nothing would be RAM here, and every request naming the
 * guest's memory would be refused. The guests this device serves have none.
 */
static bool apple_m2scaler_map(AppleM2Scaler *s, uint64_t address,
                               uint64_t length, bool is_write,
                               AppleM2ScalerRange *r)
{
    AddressSpace *as = VIRTIO_DEVICE(s)->dma_as;
    hwaddr at = address, left = length ? length : 1;

    r->count = 0;
    if (at + left < at) {
        return false;
    }
    while (left) {
        MemoryRegionCache *piece;
        MemoryRegion *mr;
        int64_t got;

        if (r->count == APPLE_M2SCALER_MAX_PIECES) {
            apple_m2scaler_unmap(r);
            return false;
        }
        piece = &r->pieces[r->count++];
        got = address_space_cache_init(piece, as, at, left, is_write);
        mr = piece->mrs.mr;
        if (got <= 0 || !piece->ptr || !memory_region_is_ram(mr) ||
            memory_region_is_ram_device(mr)) {
            apple_m2scaler_unmap(r);
            return false;
        }
        r->lengths[r->count - 1] = got;
        at += got;
        left -= got;
    }
    return true;
}

/* The library's is_ram, read and write: on the worker. */
static bool apple_m2scaler_is_ram(void *ctx, uint64_t address,
                                  uint64_t length)
{
    AppleM2ScalerRange r;

    if (!apple_m2scaler_map(ctx, address, length, false, &r)) {
        return false;
    }
    apple_m2scaler_unmap(&r);
    return true;
}

static bool apple_m2scaler_read(void *ctx, uint64_t address, uint8_t *bytes,
                                size_t length)
{
    AppleM2ScalerRange r;
    size_t at = 0;

    if (!length) {
        return apple_m2scaler_is_ram(ctx, address, 0);
    }
    if (!apple_m2scaler_map(ctx, address, length, false, &r)) {
        return false;
    }
    for (unsigned i = 0; i < r.count; i++) {
        address_space_read_cached(&r.pieces[i], 0, bytes + at, r.lengths[i]);
        at += r.lengths[i];
    }
    apple_m2scaler_unmap(&r);
    return true;
}

static bool apple_m2scaler_write(void *ctx, uint64_t address,
                                 const uint8_t *bytes, size_t length)
{
    AppleM2ScalerRange r;
    size_t at = 0;

    if (!length) {
        return apple_m2scaler_is_ram(ctx, address, 0);
    }
    if (!apple_m2scaler_map(ctx, address, length, true, &r)) {
        return false;
    }
    for (unsigned i = 0; i < r.count; i++) {
        address_space_write_cached(&r.pieces[i], 0, bytes + at, r.lengths[i]);
        address_space_cache_invalidate(&r.pieces[i], 0, r.lengths[i]);
        at += r.lengths[i];
    }
    apple_m2scaler_unmap(&r);
    return true;
}

static void apple_m2scaler_job_free(AppleM2ScalerJob *job)
{
    g_free(job->request);
    g_free(job->elem);
    g_free(job);
}

/*
 * The worker: takes each job, has the library answer it, and hands the
 * answer to the main loop. An end of the VM ends the process here, as Apple's
 * host ends it.
 */
static void *apple_m2scaler_worker(void *opaque)
{
    AppleM2Scaler *s = opaque;

    rcu_register_thread();
    for (;;) {
        AppleM2ScalerJob *job;

        WITH_QEMU_LOCK_GUARD(&s->lock) {
            while (!s->job && !s->ending) {
                qemu_cond_wait(&s->wake, &s->lock);
            }
            job = s->job;
        }
        if (!job) {
            break;
        }
        trace_apple_m2scaler_work(job->length);
        job->outcome = rvg_scaler_device_process(s->device, job->request,
                                                 job->length, job->room,
                                                 job->reason,
                                                 sizeof(job->reason));
        trace_apple_m2scaler_request(
            job->length, job->room, job->outcome,
            job->length >= 8 ? ldl_le_p(job->request) : 0,
            job->length >= 8 ? ldl_le_p(job->request + 4) : 0);
        if (job->outcome == RVG_SCALER_END_VM) {
            error_report("%s: %s", TYPE_APPLE_M2SCALER_PCI, job->reason);
            abort();
        }
        WITH_QEMU_LOCK_GUARD(&s->lock) {
            s->job = NULL;
            s->done = job;
            qemu_cond_broadcast(&s->answered);
        }
        qemu_bh_schedule(s->answered_bh);
    }
    rcu_unregister_thread();
    return NULL;
}

/*
 * Pops the next element and hands it to the worker, unless one is with it
 * or awaits its return. Under the BQL.
 */
static void apple_m2scaler_next(AppleM2Scaler *s)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    VirtQueue *vq = s->requests;
    VirtQueueElement *elem;
    AppleM2ScalerJob *job;
    size_t length, room;

    if (s->queued || vdev->broken ||
        !(elem = virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
        return;
    }
    length = iov_size(elem->out_sg, elem->out_num);
    room = iov_size(elem->in_sg, elem->in_num);
    if (length > APPLE_M2SCALER_MAX_REQUEST) {
        trace_apple_m2scaler_request(length, room, RVG_SCALER_NEEDS_RESET, 0, 0);
        virtqueue_detach_element(vq, elem, 0);
        g_free(elem);
        virtio_error(vdev, "%s: Request too large", TYPE_APPLE_M2SCALER_PCI);
        return;
    }
    job = g_new0(AppleM2ScalerJob, 1);
    job->elem = elem;
    job->length = length;
    job->room = room;
    job->request = g_malloc(MAX(length, 1));
    iov_to_buf(elem->out_sg, elem->out_num, 0, job->request, length);
    s->queued = true;
    WITH_QEMU_LOCK_GUARD(&s->lock) {
        s->job = job;
        qemu_cond_signal(&s->wake);
    }
}

/* The worker's answer, returned to the guest; then the next element. */
static void apple_m2scaler_answered_bh(void *opaque)
{
    AppleM2Scaler *s = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    VirtQueue *vq = s->requests;
    AppleM2ScalerJob *job;

    WITH_QEMU_LOCK_GUARD(&s->lock) {
        job = s->done;
        s->done = NULL;
    }
    if (!job) {
        return;
    }
    s->queued = false;
    if (job->outcome == RVG_SCALER_REPLY) {
        iov_from_buf(job->elem->in_sg, job->elem->in_num, 0, job->request,
                     job->length);
        virtqueue_push(vq, job->elem, job->length);
        virtio_notify(vdev, vq);
        apple_m2scaler_job_free(job);
        apple_m2scaler_next(s);
        return;
    }
    virtqueue_detach_element(vq, job->elem, 0);
    virtio_error(vdev, "%s: %s", TYPE_APPLE_M2SCALER_PCI, job->reason);
    apple_m2scaler_job_free(job);
}

/*
 * Waits for the worker to answer the element it has, and discards the
 * answer: for a reset, which empties the queue after this, and for the
 * device's end. Under the BQL, which the worker never takes, so a reset
 * during a transform waits that transform out.
 */
static void apple_m2scaler_drain(AppleM2Scaler *s)
{
    AppleM2ScalerJob *job;

    WITH_QEMU_LOCK_GUARD(&s->lock) {
        while (s->job) {
            qemu_cond_wait(&s->answered, &s->lock);
        }
        job = s->done;
        s->done = NULL;
    }
    qemu_bh_cancel(s->answered_bh);
    trace_apple_m2scaler_drain(job != NULL);
    if (job) {
        virtqueue_detach_element(s->requests, job->elem, 0);
        apple_m2scaler_job_free(job);
    }
    s->queued = false;
}

/*
 * The queue's notification: each element is one request, answered in turn
 * on the worker.
 */
static void apple_m2scaler_handle_requests(VirtIODevice *vdev, VirtQueue *vq)
{
    apple_m2scaler_next(APPLE_M2SCALER(vdev));
}

static void apple_m2scaler_start_failed_bh(void *opaque)
{
    virtio_error(VIRTIO_DEVICE(opaque), "%s: IOSurfaceAcceleratorCreate failed",
                 TYPE_APPLE_M2SCALER_PCI);
}

/* No features of the device's own. */
static uint64_t apple_m2scaler_get_features(VirtIODevice *vdev,
                                            uint64_t features, Error **errp)
{
    return features;
}

/*
 * DRIVER_OK makes the accelerator. A failure is a needs-reset, set once the
 * status is: virtio_set_status writes the status after this returns.
 */
static int apple_m2scaler_set_status(VirtIODevice *vdev, uint8_t status)
{
    AppleM2Scaler *s = APPLE_M2SCALER(vdev);
    bool was = vdev->status & VIRTIO_CONFIG_S_DRIVER_OK;
    bool now = status & VIRTIO_CONFIG_S_DRIVER_OK;

    trace_apple_m2scaler_set_status(status);
    if (now && !was && !rvg_scaler_device_start(s->device)) {
        qemu_bh_schedule(s->start_failed_bh);
    }
    return 0;
}

/* The guest's reset, and the machine's: the accelerator goes. */
static void apple_m2scaler_reset(VirtIODevice *vdev)
{
    AppleM2Scaler *s = APPLE_M2SCALER(vdev);

    trace_apple_m2scaler_reset();
    qemu_bh_cancel(s->start_failed_bh);
    if (s->device) {
        apple_m2scaler_drain(s);
        rvg_scaler_device_reset(s->device);
    }
}

/*
 * FEATURES_OK: refused where the library has no video processing for the
 * device, so that the guest's driver gives the device up before DRIVER_OK
 * (above).
 */
static int apple_m2scaler_validate_features(VirtIODevice *vdev)
{
    AppleM2Scaler *s = APPLE_M2SCALER(vdev);

    if (!rvg_scaler_device_scales(s->device)) {
        trace_apple_m2scaler_features_refused();
        return -ENOTSUP;
    }
    return 0;
}

static void apple_m2scaler_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    AppleM2Scaler *s = APPLE_M2SCALER(dev);
    RvgScalerGuestMemory memory = {
        .struct_size = sizeof(memory),
        .ctx = s,
        .is_ram = apple_m2scaler_is_ram,
        .read = apple_m2scaler_read,
        .write = apple_m2scaler_write,
    };
    RvgScalerDeviceConfig config = {
        .struct_size = sizeof(config),
        .iosurface_device = NULL, /* the process's latest */
        .guest_memory = &memory,
    };
    RvgStatus status;

    status = rvg_scaler_device_create(&config, &s->device);
    if (status != RVG_OK) {
        error_setg(errp, "%s: rvg_scaler_device_create: %s",
                   TYPE_APPLE_M2SCALER_PCI, rvg_status_message(status));
        return;
    }
    s->start_failed_bh = qemu_bh_new_guarded(apple_m2scaler_start_failed_bh,
                                             s, &dev->mem_reentrancy_guard);
    s->answered_bh = qemu_bh_new_guarded(apple_m2scaler_answered_bh, s,
                                         &dev->mem_reentrancy_guard);
    qemu_mutex_init(&s->lock);
    qemu_cond_init(&s->wake);
    qemu_cond_init(&s->answered);
    qemu_thread_create(&s->worker, "apple-m2scaler", apple_m2scaler_worker, s,
                       QEMU_THREAD_JOINABLE);

    /*
     * The virtio device ID is QEMU's alone: the guest reads Apple's PCI IDs,
     * and nothing in QEMU keys behaviour on this one.
     */
    virtio_init(vdev, VIRTIO_ID_VIDEO_ENCODER, 0);
    s->requests = virtio_add_queue(vdev, APPLE_M2SCALER_QUEUE_SIZE,
                                   apple_m2scaler_handle_requests);
}

static void apple_m2scaler_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    AppleM2Scaler *s = APPLE_M2SCALER(dev);

    apple_m2scaler_drain(s);
    WITH_QEMU_LOCK_GUARD(&s->lock) {
        s->ending = true;
        qemu_cond_signal(&s->wake);
    }
    qemu_thread_join(&s->worker);
    qemu_cond_destroy(&s->answered);
    qemu_cond_destroy(&s->wake);
    qemu_mutex_destroy(&s->lock);
    qemu_bh_delete(s->answered_bh);
    rvg_scaler_device_release(s->device);
    s->device = NULL;
    qemu_bh_delete(s->start_failed_bh);
    virtio_delete_queue(s->requests);
    virtio_cleanup(vdev);
}

/* The accelerator lives in the library: reims-vgpu's QEMU migrates none. */
static const VMStateDescription vmstate_apple_m2scaler = {
    .name = TYPE_APPLE_M2SCALER,
    .unmigratable = 1,
};

static void apple_m2scaler_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_apple_m2scaler;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = apple_m2scaler_realize;
    vdc->unrealize = apple_m2scaler_unrealize;
    vdc->get_features = apple_m2scaler_get_features;
    vdc->set_status = apple_m2scaler_set_status;
    vdc->validate_features = apple_m2scaler_validate_features;
    vdc->reset = apple_m2scaler_reset;
}

static const TypeInfo apple_m2scaler_info = {
    .name          = TYPE_APPLE_M2SCALER,
    .parent        = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(AppleM2Scaler),
    .class_init    = apple_m2scaler_class_init,
};

/* ------ The PCI device ------ */

static const Property apple_m2scaler_pci_properties[] = {
    DEFINE_PROP_BIT("ioeventfd", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_USE_IOEVENTFD_BIT, true),
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors,
                       DEV_NVECTORS_UNSPECIFIED),
};

static void apple_m2scaler_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    AppleM2ScalerPCI *dev = APPLE_M2SCALER_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);
    uint8_t *config = vpci_dev->pci_dev.config;
    bool capabilities[PCI_CONFIG_SPACE_SIZE] = { false };
    unsigned offset, previous = PCI_CAPABILITY_LIST;

    /* One MSI-X vector for the queue, and one for the configuration. */
    if (vpci_dev->nvectors == DEV_NVECTORS_UNSPECIFIED) {
        vpci_dev->nvectors = 2;
    }
    virtio_pci_force_virtio_1(vpci_dev);
    if (!qdev_realize(vdev, BUS(&vpci_dev->bus), errp)) {
        return;
    }

    /*
     * virtio-pci sets the device ID of a modern virtio device from its
     * virtio ID: Apple's IDs again.
     */
    pci_config_set_vendor_id(config, RVG_SCALER_PCI_VENDOR_ID);
    pci_config_set_device_id(config, RVG_SCALER_PCI_DEVICE_ID);

    /*
     * The capabilities linked in ascending address order, as
     * apple-videotoolbox-pci links them for macOS 27's virtio transport.
     */
    for (offset = config[PCI_CAPABILITY_LIST]; offset;
         offset = config[offset + PCI_CAP_LIST_NEXT]) {
        capabilities[offset] = true;
    }
    for (offset = PCI_CONFIG_HEADER_SIZE; offset < PCI_CONFIG_SPACE_SIZE;
         offset++) {
        if (capabilities[offset]) {
            config[previous] = offset;
            previous = offset + PCI_CAP_LIST_NEXT;
        }
    }
    config[previous] = 0;
}

static void apple_m2scaler_pci_class_init(ObjectClass *klass,
                                          const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    dc->desc = "macOS Paravirtualized M2 Scaler";
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, apple_m2scaler_pci_properties);
    k->realize = apple_m2scaler_pci_realize;
    pcidev_k->vendor_id = RVG_SCALER_PCI_VENDOR_ID;
    pcidev_k->device_id = RVG_SCALER_PCI_DEVICE_ID;
    /*
     * Apple's: the subsystem 106b:1a0b and the class of a processing
     * accelerator, 0x12, subclass 0.
     */
    pcidev_k->subsystem_vendor_id = RVG_SCALER_PCI_VENDOR_ID;
    pcidev_k->subsystem_id = RVG_SCALER_PCI_DEVICE_ID;
    pcidev_k->revision = VIRTIO_PCI_ABI_VERSION;
    pcidev_k->class_id = 0x1200;
}

static void apple_m2scaler_pci_instance_init(Object *obj)
{
    AppleM2ScalerPCI *dev = APPLE_M2SCALER_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_APPLE_M2SCALER);
}

static const VirtioPCIDeviceTypeInfo apple_m2scaler_pci_info = {
    .generic_name  = TYPE_APPLE_M2SCALER_PCI,
    .instance_size = sizeof(AppleM2ScalerPCI),
    .instance_init = apple_m2scaler_pci_instance_init,
    .class_init    = apple_m2scaler_pci_class_init,
};

static void apple_m2scaler_register_types(void)
{
    type_register_static(&apple_m2scaler_info);
    virtio_pci_types_register(&apple_m2scaler_pci_info);
}

type_init(apple_m2scaler_register_types)
