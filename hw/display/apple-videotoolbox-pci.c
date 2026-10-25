/*
 * QEMU Apple paravirtualized VideoToolbox device, virtio-PCI 106b:1a03
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A macOS guest's AppleVideoToolboxParavirtualization kext hands its
 * VideoToolbox's decoder, encoder and motion-estimation sessions to the host
 * through this device, as Virtualization.framework gives it. Its host side is
 * reims-vgpu's video device (rvg_video_device_*, reims_vgpu.h); this file is
 * its transport, a modern (1.x) virtio device with Apple's IDs, no device
 * features and no device configuration:
 *
 *  - Queue 0, guest to host. Each element is one message: its readable part is
 *    the message, which the library's host session takes whole, and its
 *    writable part takes the session's int32 status, little-endian. A
 *    writable part shorter than 4 bytes is logged and gets no status. A
 *    readable part longer than RVG_VIDEO_MAX_MESSAGE_SIZE is answered
 *    RVG_VIDEO_ALLOCATION_FAILED here, as the library answers it, and never
 *    copied: a guest's descriptors may add up to many times the host's
 *    memory. It holds 1024 descriptors, where Apple's queues hold 256
 *    (below).
 *  - The library answers each message on the device's worker thread, not the
 *    main loop, as Apple's host answers them on the device's own queue: a
 *    JPEG decode takes the GPU for as long as it takes, and a session's end
 *    may wait for its thread, and the VM's main loop runs meanwhile. One
 *    element is with the worker at a time; its status goes back on the main
 *    loop, and only then is the next popped, so the elements are answered
 *    one by one in the guest's order.
 *  - Queue 1, host to guest. The guest posts buffers of 0x4000 bytes. The
 *    library's messages, which arrive on its threads, wait in a ring here, and
 *    each fills the next buffer the guest posts, in the order they came. A
 *    notification on queue 1 only drains the ring. A message longer than the
 *    guest's buffer ends the process, as Apple's host ends the VM. The ring
 *    is not drained while an element is with the worker: a message reaches
 *    the guest after the status of the element being answered as it was
 *    sent, as on Apple's host, whose replies are queued behind the handler
 *    on the device's queue.
 *
 * The IOSurfaces a message names are resolved by the library, through the
 * process's latest IOSurface device (apple-gfx-iosurface-pci on q35,
 * apple-gfx-mmio on vmapple); without one, they resolve to none.
 *
 * DRIVER_OK starts the library's host session; a reset ends it, once the
 * element with the worker is answered, which the reset then discards, and
 * once the session's work has drained. The ring of host messages is kept
 * across the reset, as Apple's host keeps its own.
 *
 * The device has no properties of its own: the library picks what it decodes
 * and encodes with, and logs its choice and why. On macOS it is VideoToolbox.
 * On Linux it runs on the GPU the library's GPU device (apple-gfx-pci) runs
 * on, which the Vulkan loader's environment picks: NVIDIA's NVDEC and NVENC
 * on an NVIDIA GPU, else VA-API on its render node, and a hardware GPU of the
 * host's where that GPU is a software rasterizer or offers no video. Where no
 * backend opens, the guest is offered no codec.
 *
 * Not for upstream: written with an AI assistant, which QEMU's policy on
 * AI-generated content (docs/devel/code-provenance.rst) declines.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/queue.h"
#include "qemu/thread.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "migration/vmstate.h"
#include "standard-headers/linux/virtio_ids.h"
#include "trace.h"
#include <reims_vgpu.h>

#define TYPE_APPLE_VIDEOTOOLBOX "apple-videotoolbox"
OBJECT_DECLARE_SIMPLE_TYPE(AppleVideoToolbox, APPLE_VIDEOTOOLBOX)

#define TYPE_APPLE_VIDEOTOOLBOX_PCI "apple-videotoolbox-pci"
OBJECT_DECLARE_SIMPLE_TYPE(AppleVideoToolboxPCI, APPLE_VIDEOTOOLBOX_PCI)

/*
 * Each queue's size. Apple's device refuses a queue of more than 256
 * descriptors when the guest enables it, as its VirtioDevice base refuses
 * every queue. The guest kext's queues take the size the device reports
 * (AppleVirtIOQueue::initWithTransport), and its messages go out as direct
 * descriptor chains, one descriptor per physical segment, a transaction of
 * more segments than the queue holds refused with kIOReturnBadArgument
 * (AppleVirtIOQueue::prepareTransaction, macOS 26.7.1's AppleVirtIO).
 * VideoToolbox cuts a sample into messages of up to 0x200000 bytes, 128
 * pages of an arm64 guest's 16 KiB, but 512 of an x86_64 guest's 4 KiB, and
 * 256 descriptors carry 1 MiB there. So a guest-to-host queue of 1024 is
 * ours, for x86_64 guests; the host-to-guest queue, whose buffers are 0x4000
 * bytes, keeps Apple's 256.
 */
#define APPLE_VIDEOTOOLBOX_GUEST_TO_HOST_QUEUE_SIZE 1024
#define APPLE_VIDEOTOOLBOX_HOST_TO_GUEST_QUEUE_SIZE 256
#define APPLE_VIDEOTOOLBOX_STATUS_SIZE 4

/* One host-to-guest message waiting for a buffer of the guest's. */
typedef struct AppleVideoToolboxMessage {
    QSIMPLEQ_ENTRY(AppleVideoToolboxMessage) next;
    size_t length;
    uint8_t bytes[];
} AppleVideoToolboxMessage;

/*
 * One element with the worker: its readable part, copied, and the session's
 * status for it.
 */
typedef struct AppleVideoToolboxJob {
    VirtQueueElement *elem;
    uint8_t *message;
    size_t length;
    int32_t status;
} AppleVideoToolboxJob;

struct AppleVideoToolbox {
    VirtIODevice parent_obj;

    VirtQueue *guest_to_host; /* queue 0 */
    VirtQueue *host_to_guest; /* queue 1 */
    RvgVideoDevice *device;

    /* The library's messages, in the order they came; ring_lock protected. */
    QemuMutex ring_lock;
    QSIMPLEQ_HEAD(, AppleVideoToolboxMessage) ring;
    /* Drains the ring into queue 1 under the BQL. */
    QEMUBH *drain_bh;

    /*
     * The worker, which calls rvg_video_device_deliver. Under lock: job, the
     * element it has or is to take (NULL once it answered), done, its answer
     * until answered_bh takes it, and ending. queued is the main loop's alone:
     * an element is with the worker or answered and not yet returned, so no
     * other is popped and the ring is not drained.
     */
    QemuThread worker;
    QemuMutex lock;
    QemuCond wake;
    QemuCond answered;
    AppleVideoToolboxJob *job;
    AppleVideoToolboxJob *done;
    bool ending;
    bool queued;
    /* Returns the worker's answer to the guest, on the main loop. */
    QEMUBH *answered_bh;
};

struct AppleVideoToolboxPCI {
    VirtIOPCIProxy parent_obj;

    AppleVideoToolbox vdev;
};

/*
 * The library's send_to_guest: on one of its threads, or on the worker inside
 * rvg_video_device_deliver. It copies the message into the ring and has the
 * main loop drain it, so it waits for nothing the BQL holds.
 */
static void apple_videotoolbox_send_to_guest(void *ctx, const uint8_t *message,
                                             size_t length)
{
    AppleVideoToolbox *s = ctx;
    AppleVideoToolboxMessage *m = g_malloc(sizeof(*m) + length);

    m->length = length;
    memcpy(m->bytes, message, length);
    WITH_QEMU_LOCK_GUARD(&s->ring_lock) {
        QSIMPLEQ_INSERT_TAIL(&s->ring, m, next);
    }
    trace_apple_videotoolbox_send_to_guest(length);
    qemu_bh_schedule(s->drain_bh);
}

/*
 * Moves the ring's messages into the guest's buffers on queue 1, while it
 * has both, and no element is with the worker: answered_bh drains once it
 * has returned that element. Under the BQL.
 */
static void apple_videotoolbox_drain(AppleVideoToolbox *s)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    if (s->queued) {
        return;
    }
    for (;;) {
        AppleVideoToolboxMessage *m;
        VirtQueueElement *elem;
        size_t room;

        WITH_QEMU_LOCK_GUARD(&s->ring_lock) {
            m = QSIMPLEQ_FIRST(&s->ring);
        }
        if (!m) {
            return;
        }
        elem = virtqueue_pop(s->host_to_guest, sizeof(VirtQueueElement));
        if (!elem) {
            return;
        }
        room = iov_size(elem->in_sg, elem->in_num);
        if (room < m->length) {
            error_report("%s: a host message of %zu bytes, and the guest's "
                         "buffer holds %zu", TYPE_APPLE_VIDEOTOOLBOX_PCI,
                         m->length, room);
            abort();
        }
        iov_from_buf(elem->in_sg, elem->in_num, 0, m->bytes, m->length);
        trace_apple_videotoolbox_to_guest(m->length, room);
        virtqueue_push(s->host_to_guest, elem, m->length);
        virtio_notify(vdev, s->host_to_guest);
        g_free(elem);
        WITH_QEMU_LOCK_GUARD(&s->ring_lock) {
            QSIMPLEQ_REMOVE_HEAD(&s->ring, next);
        }
        g_free(m);
    }
}

static void apple_videotoolbox_drain_bh(void *opaque)
{
    apple_videotoolbox_drain(opaque);
}

/* Queue 1's notification: the guest posted buffers. */
static void apple_videotoolbox_handle_host_to_guest(VirtIODevice *vdev,
                                                    VirtQueue *vq)
{
    apple_videotoolbox_drain(APPLE_VIDEOTOOLBOX(vdev));
}

static void apple_videotoolbox_job_free(AppleVideoToolboxJob *job)
{
    g_free(job->message);
    g_free(job->elem);
    g_free(job);
}

/*
 * The worker: takes each job, has the library's host session answer it, and
 * hands the status to the main loop. It never takes the BQL: the library's
 * calls back, send_to_guest and the IOSurface device's, wait for nothing that
 * holds it.
 */
static void *apple_videotoolbox_worker(void *opaque)
{
    AppleVideoToolbox *s = opaque;

    for (;;) {
        AppleVideoToolboxJob *job;

        WITH_QEMU_LOCK_GUARD(&s->lock) {
            while (!s->job && !s->ending) {
                qemu_cond_wait(&s->wake, &s->lock);
            }
            job = s->job;
        }
        if (!job) {
            break;
        }
        trace_apple_videotoolbox_work(job->length);
        job->status = rvg_video_device_deliver(s->device, job->message,
                                               job->length);
        WITH_QEMU_LOCK_GUARD(&s->lock) {
            s->job = NULL;
            s->done = job;
            qemu_cond_broadcast(&s->answered);
        }
        qemu_bh_schedule(s->answered_bh);
    }
    return NULL;
}

/*
 * Writes `status` into `elem`, a message of `length` bytes, and returns it to
 * the guest. Under the BQL.
 */
static void apple_videotoolbox_answer(AppleVideoToolbox *s,
                                      VirtQueueElement *elem, size_t length,
                                      int32_t status)
{
    size_t room = iov_size(elem->in_sg, elem->in_num);
    unsigned written = 0;

    trace_apple_videotoolbox_from_guest(length, room, status);
    if (room < APPLE_VIDEOTOOLBOX_STATUS_SIZE) {
        warn_report_once("%s: a guest message with no room for its status "
                         "(%zu bytes)", TYPE_APPLE_VIDEOTOOLBOX_PCI, room);
    } else {
        uint32_t le = cpu_to_le32((uint32_t)status);

        iov_from_buf(elem->in_sg, elem->in_num, 0, &le, sizeof(le));
        written = sizeof(le);
    }
    virtqueue_push(s->guest_to_host, elem, written);
    virtio_notify(VIRTIO_DEVICE(s), s->guest_to_host);
}

/*
 * Pops the next element and hands it to the worker, unless one is with it
 * or awaits its return; one too long for the library is answered here, unread,
 * and the next popped. Under the BQL.
 */
static void apple_videotoolbox_next(AppleVideoToolbox *s)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    VirtQueueElement *elem;
    AppleVideoToolboxJob *job;
    size_t length;

    for (;;) {
        if (s->queued || vdev->broken ||
            !(elem = virtqueue_pop(s->guest_to_host,
                                   sizeof(VirtQueueElement)))) {
            return;
        }
        length = iov_size(elem->out_sg, elem->out_num);
        if (length <= RVG_VIDEO_MAX_MESSAGE_SIZE) {
            break;
        }
        warn_report_once("%s: a guest message of %zu bytes, over the %u the "
                         "library takes: answered %d, unread",
                         TYPE_APPLE_VIDEOTOOLBOX_PCI, length,
                         (unsigned)RVG_VIDEO_MAX_MESSAGE_SIZE,
                         RVG_VIDEO_ALLOCATION_FAILED);
        apple_videotoolbox_answer(s, elem, length,
                                  RVG_VIDEO_ALLOCATION_FAILED);
        g_free(elem);
    }
    job = g_new0(AppleVideoToolboxJob, 1);
    job->elem = elem;
    job->length = length;
    job->message = g_malloc(MAX(job->length, 1));
    iov_to_buf(elem->out_sg, elem->out_num, 0, job->message, job->length);
    s->queued = true;
    WITH_QEMU_LOCK_GUARD(&s->lock) {
        s->job = job;
        qemu_cond_signal(&s->wake);
    }
}

/*
 * The worker's status, written into the element, which goes back to the
 * guest; then what the session sent while answering it, and the next element.
 */
static void apple_videotoolbox_answered_bh(void *opaque)
{
    AppleVideoToolbox *s = opaque;
    AppleVideoToolboxJob *job;

    WITH_QEMU_LOCK_GUARD(&s->lock) {
        job = s->done;
        s->done = NULL;
    }
    if (!job) {
        return;
    }
    apple_videotoolbox_answer(s, job->elem, job->length, job->status);
    apple_videotoolbox_job_free(job);
    s->queued = false;
    apple_videotoolbox_drain(s);
    apple_videotoolbox_next(s);
}

/*
 * Waits for the worker to answer the element it has, and discards the
 * answer: for a reset, which empties the queue after this, and for the
 * device's end. Under the BQL, which the worker never takes, so it cannot
 * deadlock; a reset during a long JPEG decode waits for it, 10 s at most, the
 * library's bound on its backend's call.
 */
static void apple_videotoolbox_drain_worker(AppleVideoToolbox *s)
{
    AppleVideoToolboxJob *job;

    WITH_QEMU_LOCK_GUARD(&s->lock) {
        while (s->job) {
            qemu_cond_wait(&s->answered, &s->lock);
        }
        job = s->done;
        s->done = NULL;
    }
    qemu_bh_cancel(s->answered_bh);
    trace_apple_videotoolbox_drain_worker(job != NULL);
    if (job) {
        virtqueue_detach_element(s->guest_to_host, job->elem, 0);
        apple_videotoolbox_job_free(job);
    }
    s->queued = false;
}

/*
 * Queue 0's notification: each element is one message for the session,
 * answered in turn on the worker.
 */
static void apple_videotoolbox_handle_guest_to_host(VirtIODevice *vdev,
                                                    VirtQueue *vq)
{
    apple_videotoolbox_next(APPLE_VIDEOTOOLBOX(vdev));
}

/* No features of the device's own. */
static uint64_t apple_videotoolbox_get_features(VirtIODevice *vdev,
                                                uint64_t features,
                                                Error **errp)
{
    return features;
}

/* DRIVER_OK starts the host session. */
static int apple_videotoolbox_set_status(VirtIODevice *vdev, uint8_t status)
{
    AppleVideoToolbox *s = APPLE_VIDEOTOOLBOX(vdev);
    bool was = vdev->status & VIRTIO_CONFIG_S_DRIVER_OK;
    bool now = status & VIRTIO_CONFIG_S_DRIVER_OK;

    trace_apple_videotoolbox_set_status(status);
    if (now && !was) {
        rvg_video_device_start(s->device);
    }
    return 0;
}

/* The guest's reset, and the machine's: the host session ends. */
static void apple_videotoolbox_reset(VirtIODevice *vdev)
{
    AppleVideoToolbox *s = APPLE_VIDEOTOOLBOX(vdev);

    trace_apple_videotoolbox_reset();
    if (s->device) {
        apple_videotoolbox_drain_worker(s);
        rvg_video_device_reset(s->device);
    }
}

static void apple_videotoolbox_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    AppleVideoToolbox *s = APPLE_VIDEOTOOLBOX(dev);
    RvgVideoCallbacks callbacks = {
        .struct_size = sizeof(callbacks),
        .ctx = s,
        .send_to_guest = apple_videotoolbox_send_to_guest,
    };
    RvgVideoDeviceConfig config = {
        .struct_size = sizeof(config),
        .iosurface_device = NULL, /* the process's latest */
        .callbacks = &callbacks,
    };
    RvgStatus status;

    qemu_mutex_init(&s->ring_lock);
    QSIMPLEQ_INIT(&s->ring);
    s->drain_bh = qemu_bh_new_guarded(apple_videotoolbox_drain_bh, s,
                                      &dev->mem_reentrancy_guard);

    status = rvg_video_device_create(&config, &s->device);
    if (status != RVG_OK) {
        error_setg(errp, "%s: rvg_video_device_create: %s",
                   TYPE_APPLE_VIDEOTOOLBOX_PCI, rvg_status_message(status));
        qemu_bh_delete(s->drain_bh);
        qemu_mutex_destroy(&s->ring_lock);
        return;
    }
    s->answered_bh = qemu_bh_new_guarded(apple_videotoolbox_answered_bh, s,
                                         &dev->mem_reentrancy_guard);
    qemu_mutex_init(&s->lock);
    qemu_cond_init(&s->wake);
    qemu_cond_init(&s->answered);
    qemu_thread_create(&s->worker, "apple-videotoolbox",
                       apple_videotoolbox_worker, s, QEMU_THREAD_JOINABLE);

    /*
     * The virtio device ID is QEMU's alone: the guest reads Apple's PCI IDs,
     * and nothing in QEMU keys behaviour on this one.
     */
    virtio_init(vdev, VIRTIO_ID_VIDEO_DECODER, 0);
    s->guest_to_host = virtio_add_queue(vdev,
                                        APPLE_VIDEOTOOLBOX_GUEST_TO_HOST_QUEUE_SIZE,
                                        apple_videotoolbox_handle_guest_to_host);
    s->host_to_guest = virtio_add_queue(vdev,
                                        APPLE_VIDEOTOOLBOX_HOST_TO_GUEST_QUEUE_SIZE,
                                        apple_videotoolbox_handle_host_to_guest);
}

static void apple_videotoolbox_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    AppleVideoToolbox *s = APPLE_VIDEOTOOLBOX(dev);
    AppleVideoToolboxMessage *m, *next;

    /* The worker first: it may be inside the library, sending. */
    apple_videotoolbox_drain_worker(s);
    WITH_QEMU_LOCK_GUARD(&s->lock) {
        s->ending = true;
        qemu_cond_signal(&s->wake);
    }
    qemu_thread_join(&s->worker);
    qemu_cond_destroy(&s->answered);
    qemu_cond_destroy(&s->wake);
    qemu_mutex_destroy(&s->lock);
    qemu_bh_delete(s->answered_bh);
    /* Its last release resets it: no send_to_guest follows. */
    rvg_video_device_release(s->device);
    s->device = NULL;
    qemu_bh_delete(s->drain_bh);
    QSIMPLEQ_FOREACH_SAFE(m, &s->ring, next, next) {
        g_free(m);
    }
    qemu_mutex_destroy(&s->ring_lock);
    virtio_delete_queue(s->guest_to_host);
    virtio_delete_queue(s->host_to_guest);
    virtio_cleanup(vdev);
}

/* The host session lives in the library: reims-vgpu's QEMU migrates none. */
static const VMStateDescription vmstate_apple_videotoolbox = {
    .name = TYPE_APPLE_VIDEOTOOLBOX,
    .unmigratable = 1,
};

static void apple_videotoolbox_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_apple_videotoolbox;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = apple_videotoolbox_realize;
    vdc->unrealize = apple_videotoolbox_unrealize;
    vdc->get_features = apple_videotoolbox_get_features;
    vdc->set_status = apple_videotoolbox_set_status;
    vdc->reset = apple_videotoolbox_reset;
}

static const TypeInfo apple_videotoolbox_info = {
    .name          = TYPE_APPLE_VIDEOTOOLBOX,
    .parent        = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(AppleVideoToolbox),
    .class_init    = apple_videotoolbox_class_init,
};

/* ------ The PCI device ------ */

static const Property apple_videotoolbox_pci_properties[] = {
    DEFINE_PROP_BIT("ioeventfd", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_USE_IOEVENTFD_BIT, true),
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors,
                       DEV_NVECTORS_UNSPECIFIED),
};

static void apple_videotoolbox_pci_realize(VirtIOPCIProxy *vpci_dev,
                                           Error **errp)
{
    AppleVideoToolboxPCI *dev = APPLE_VIDEOTOOLBOX_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);
    uint8_t *config = vpci_dev->pci_dev.config;
    bool capabilities[PCI_CONFIG_SPACE_SIZE] = { false };
    unsigned offset, previous = PCI_CAPABILITY_LIST;

    /* One MSI-X vector per queue, and one for the configuration. */
    if (vpci_dev->nvectors == DEV_NVECTORS_UNSPECIFIED) {
        vpci_dev->nvectors = 3;
    }
    virtio_pci_force_virtio_1(vpci_dev);
    if (!qdev_realize(vdev, BUS(&vpci_dev->bus), errp)) {
        return;
    }

    /*
     * virtio-pci sets the device ID of a modern virtio device from its
     * virtio ID: Apple's IDs again.
     */
    pci_config_set_vendor_id(config, RVG_VIDEO_PCI_VENDOR_ID);
    pci_config_set_device_id(config, RVG_VIDEO_PCI_DEVICE_ID);

    /*
     * The capabilities linked in ascending address order, each at its offset
     * with its contents: macOS 27's virtio transport never starts a device
     * whose list runs from high offsets to low, as QEMU links it, and macOS
     * 26's walks either (vmapple-virtio-blk-pci does the same).
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

static void apple_videotoolbox_pci_class_init(ObjectClass *klass,
                                              const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    dc->desc = "macOS Paravirtualized VideoToolbox";
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, apple_videotoolbox_pci_properties);
    k->realize = apple_videotoolbox_pci_realize;
    pcidev_k->vendor_id = RVG_VIDEO_PCI_VENDOR_ID;
    pcidev_k->device_id = RVG_VIDEO_PCI_DEVICE_ID;
    /*
     * Ours, as Apple's are not read: the subsystem as the scaler device's,
     * 106b and its device ID, and the class of a processing accelerator
     * (0x12), as the scaler device's.
     */
    pcidev_k->subsystem_vendor_id = RVG_VIDEO_PCI_VENDOR_ID;
    pcidev_k->subsystem_id = RVG_VIDEO_PCI_DEVICE_ID;
    pcidev_k->revision = VIRTIO_PCI_ABI_VERSION;
    pcidev_k->class_id = 0x1200;
}

static void apple_videotoolbox_pci_instance_init(Object *obj)
{
    AppleVideoToolboxPCI *dev = APPLE_VIDEOTOOLBOX_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_APPLE_VIDEOTOOLBOX);
}

static const VirtioPCIDeviceTypeInfo apple_videotoolbox_pci_info = {
    .generic_name  = TYPE_APPLE_VIDEOTOOLBOX_PCI,
    .instance_size = sizeof(AppleVideoToolboxPCI),
    .instance_init = apple_videotoolbox_pci_instance_init,
    .class_init    = apple_videotoolbox_pci_class_init,
};

static void apple_videotoolbox_register_types(void)
{
    type_register_static(&apple_videotoolbox_info);
    virtio_pci_types_register(&apple_videotoolbox_pci_info);
}

type_init(apple_videotoolbox_register_types)
