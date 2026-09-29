/*
 * QEMU Apple ParavirtualizedGraphics.framework device, MMIO (arm64) variant
 *
 * Copyright © 2023 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ParavirtualizedGraphics.framework is a set of libraries that macOS provides
 * which implements 3d graphics passthrough to the host as well as a
 * proprietary guest communication channel to drive it. This device model
 * implements support to drive that library from within QEMU as an MMIO-based
 * system device for macOS on arm64 VMs.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/aio-wait.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "apple-gfx.h"
#include "trace.h"
#include "system/address-spaces.h"


OBJECT_DECLARE_SIMPLE_TYPE(AppleGFXMMIOState, APPLE_GFX_MMIO)


struct AppleGFXMapSurfaceMemoryJob;
struct AppleGFXMMIOState {
    SysBusDevice parent_obj;

    AppleGFXState common;

    qemu_irq irq_gfx;
    qemu_irq irq_iosfc;
    MemoryRegion iomem_iosfc;
    RvgIOSurfaceHostDevice *pgiosfc;
};

typedef struct AppleGFXMMIOJob {
    AppleGFXMMIOState *state;
    uint64_t offset;
    uint64_t value;
    bool completed;
} AppleGFXMMIOJob;

static void iosfc_do_read(void *opaque)
{
    AppleGFXMMIOJob *job = opaque;
    /*
     * QEMU declared -mmioReadAtOffset: as returning uint32_t, so the value
     * is truncated to 32 bits, then zero-extended into job->value.
     */
    job->value = (uint32_t)rvg_iosurface_host_device_mmio_read(
        job->state->pgiosfc, job->offset);
    qatomic_set(&job->completed, true);
    aio_wait_kick();
}

static uint64_t iosfc_read(void *opaque, hwaddr offset, unsigned size)
{
    AppleGFXMMIOJob job = {
        .state = opaque,
        .offset = offset,
        .completed = false,
    };
    dispatch_queue_t queue =
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);

    dispatch_async_f(queue, &job, iosfc_do_read);
    AIO_WAIT_WHILE(NULL, !qatomic_read(&job.completed));

    trace_apple_gfx_mmio_iosfc_read(offset, job.value);
    return job.value;
}

static void iosfc_do_write(void *opaque)
{
    AppleGFXMMIOJob *job = opaque;
    /* QEMU declared value:(uint32_t): the value is truncated to 32 bits. */
    rvg_iosurface_host_device_mmio_write(job->state->pgiosfc, job->offset,
                                         (uint32_t)job->value);
    qatomic_set(&job->completed, true);
    aio_wait_kick();
}

static void iosfc_write(void *opaque, hwaddr offset, uint64_t val,
                        unsigned size)
{
    AppleGFXMMIOJob job = {
        .state = opaque,
        .offset = offset,
        .value = val,
        .completed = false,
    };
    dispatch_queue_t queue =
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);

    dispatch_async_f(queue, &job, iosfc_do_write);
    AIO_WAIT_WHILE(NULL, !qatomic_read(&job.completed));

    trace_apple_gfx_mmio_iosfc_write(offset, val);
}

static const MemoryRegionOps apple_iosfc_ops = {
    .read = iosfc_read,
    .write = iosfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void raise_irq_bh(void *opaque)
{
    qemu_irq *irq = opaque;

    qemu_irq_pulse(*irq);
}

static void *apple_gfx_mmio_map_surface_memory(uint64_t guest_physical_address,
                                               uint64_t length, bool read_only)
{
    void *mem;
    MemoryRegion *region = NULL;

    RCU_READ_LOCK_GUARD();
    mem = apple_gfx_host_ptr_for_gpa_range(guest_physical_address,
                                           length, read_only, &region);
    if (mem) {
        memory_region_ref(region);
    }
    return mem;
}

static bool apple_gfx_mmio_unmap_surface_memory(void *ptr)
{
    MemoryRegion *region;
    ram_addr_t offset = 0;

    RCU_READ_LOCK_GUARD();
    region = memory_region_from_host(ptr, &offset);
    if (!region) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: memory at %p to be unmapped not found.\n",
                      __func__, ptr);
        return false;
    }

    trace_apple_gfx_iosfc_unmap_memory_region(ptr, region);
    memory_region_unref(region);
    return true;
}

/*
 * The Objective-C version's blocks, as {fn, ctx, destroy} triples: each body
 * is the block's, ctx is what the block captured (nothing for the map and
 * unmap blocks), and destroy is NULL because releasing the blocks released
 * nothing.
 */
static bool apple_gfx_mmio_iosfc_map_memory_cb(void *ctx, uint64_t phys,
                                               uint64_t len, bool ro,
                                               void **va, void *e, void *f)
{
    *va = apple_gfx_mmio_map_surface_memory(phys, len, ro);

    trace_apple_gfx_iosfc_map_memory(phys, len, ro, va, e, f, *va);

    return *va != NULL;
}

static bool apple_gfx_mmio_iosfc_unmap_memory_cb(void *ctx, void *va, void *b,
                                                 void *c, void *d, void *e,
                                                 void *f)
{
    return apple_gfx_mmio_unmap_surface_memory(va);
}

/* The block returned true (IOSFCRaiseInterrupt); RvgRaiseInterruptFn is void. */
static void apple_gfx_mmio_iosfc_raise_interrupt_cb(void *ctx, uint32_t vector)
{
    AppleGFXMMIOState *s = ctx;
    trace_apple_gfx_iosfc_raise_irq(vector);
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            raise_irq_bh, &s->irq_iosfc);
}

static RvgIOSurfaceHostDevice *apple_gfx_prepare_iosurface_host_device(
    AppleGFXMMIOState *s)
{
    RvgIOSurfaceHostDeviceDescriptor *iosfc_desc =
        rvg_iosurface_host_device_descriptor_create();
    RvgIOSurfaceHostDevice *iosfc_host_dev;

    /*
     * The legacy memory management API is no longer present
     * for the IOSurface mapper as of macOS 15.4.
     */
    if (@available(macOS 15.4, *)) {
        RvgMemoryMapDescriptor *memory_map_descriptor =
            rvg_memory_map_descriptor_create();
        FlatView* fv = address_space_to_flatview(&address_space_memory);
        flatview_for_each_range(fv, apple_gfx_register_memory_cb, memory_map_descriptor);
        /* the device model defines this as a single-page MMIO region, hence 16KB */
        rvg_iosurface_host_device_descriptor_set_mmio_length(iosfc_desc,
                                                             0x10000);
        /* QEMU's own reference to the map is never released, as before. */
        rvg_iosurface_host_device_descriptor_set_memory_map(
            iosfc_desc, memory_map_descriptor);
    } else {
        rvg_iosurface_host_device_descriptor_set_map_memory(
            iosfc_desc, apple_gfx_mmio_iosfc_map_memory_cb, NULL, NULL);

        rvg_iosurface_host_device_descriptor_set_unmap_memory(
            iosfc_desc, apple_gfx_mmio_iosfc_unmap_memory_cb, NULL, NULL);
    }

    rvg_iosurface_host_device_descriptor_set_raise_interrupt(
        iosfc_desc, apple_gfx_mmio_iosfc_raise_interrupt_cb, s, NULL);

    iosfc_host_dev = rvg_iosurface_host_device_create(iosfc_desc);
    rvg_iosurface_host_device_descriptor_release(iosfc_desc);
    return iosfc_host_dev;
}

static void apple_gfx_mmio_raise_interrupt_cb(void *ctx, uint32_t vector)
{
    AppleGFXMMIOState *s = ctx;
    trace_apple_gfx_raise_irq(vector);
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            raise_irq_bh, &s->irq_gfx);
}

static void apple_gfx_mmio_realize(DeviceState *dev, Error **errp)
{
    @autoreleasepool {
        AppleGFXMMIOState *s = APPLE_GFX_MMIO(dev);
        RvgDeviceDescriptor *desc = rvg_device_descriptor_create();

        rvg_device_descriptor_set_raise_interrupt(
            desc, apple_gfx_mmio_raise_interrupt_cb, s, NULL);

        rvg_device_descriptor_set_using_iosurface_mapper(desc, true);
        rvg_device_descriptor_set_enable_argument_buffers(desc, true);
        /*
         * Process isolation needs PGMemoryMapDescriptor instead of
         * the legacy memory management interface present in releases
         * older than macOS 15.4.
         */
        if (@available(macOS 15.4, *)) {
            rvg_device_descriptor_set_enable_process_isolation(desc, true);
        }

        if (!apple_gfx_common_realize(&s->common, dev, desc, errp)) {
            rvg_iosurface_host_device_release(s->pgiosfc);
            s->pgiosfc = NULL;
        }

        s->pgiosfc = apple_gfx_prepare_iosurface_host_device(s);

        rvg_device_descriptor_release(desc);
        desc = NULL;
    }
}

static void apple_gfx_mmio_init(Object *obj)
{
    AppleGFXMMIOState *s = APPLE_GFX_MMIO(obj);

    apple_gfx_common_init(obj, &s->common, TYPE_APPLE_GFX_MMIO);

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->common.iomem_gfx);
    memory_region_init_io(&s->iomem_iosfc, obj, &apple_iosfc_ops, s,
                          TYPE_APPLE_GFX_MMIO, 0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem_iosfc);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq_gfx);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq_iosfc);
}

static void apple_gfx_mmio_reset(Object *obj, ResetType type)
{
    AppleGFXMMIOState *s = APPLE_GFX_MMIO(obj);
    rvg_device_reset(s->common.pgdev);
}

static const Property apple_gfx_mmio_properties[] = {
    DEFINE_PROP_ARRAY("display-modes", AppleGFXMMIOState,
                      common.num_display_modes, common.display_modes,
                      qdev_prop_apple_gfx_display_mode, AppleGFXDisplayMode),
};

static void apple_gfx_mmio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = apple_gfx_mmio_reset;
    dc->hotpluggable = false;
    dc->realize = apple_gfx_mmio_realize;

    device_class_set_props(dc, apple_gfx_mmio_properties);
}

static const TypeInfo apple_gfx_mmio_types[] = {
    {
        .name          = TYPE_APPLE_GFX_MMIO,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(AppleGFXMMIOState),
        .class_init    = apple_gfx_mmio_class_init,
        .instance_init = apple_gfx_mmio_init,
    }
};
DEFINE_TYPES(apple_gfx_mmio_types)
