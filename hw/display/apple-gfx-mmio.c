/*
 * QEMU Apple paravirtualized graphics device, MMIO (arm64) variant
 *
 * Copyright © 2023 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * macOS guests drive a paravirtualized GPU, the device of Apple's
 * ParavirtualizedGraphics.framework, which implements 3d graphics passthrough
 * to the host as well as a proprietary guest communication channel. This
 * device model drives reims-vgpu, a library that implements the host side of
 * that device, from within QEMU as an MMIO-based system device for macOS on
 * arm64 VMs. Its second MMIO region is the IOSurface device, which maps the
 * guest's IOSurfaces.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/aio-wait.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "apple-gfx.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(AppleGFXMMIOState, APPLE_GFX_MMIO)

struct AppleGFXMMIOState {
    SysBusDevice parent_obj;

    AppleGFXState common;

    qemu_irq irq_gfx;
    qemu_irq irq_iosfc;
    MemoryRegion iomem_iosfc;
    RvgIOSurfaceDevice *pgiosfc;
};

typedef struct AppleGFXMMIOJob {
    AppleGFXMMIOState *state;
    uint64_t offset;
    uint64_t value;
    bool completed;
} AppleGFXMMIOJob;

/*
 * The IOSurface device's registers are 64 bits wide, and QEMU reads and
 * writes them as 32: a value read is truncated, then zero-extended into
 * job->value, and a value written is truncated.
 */
static int iosfc_do_read(void *opaque)
{
    AppleGFXMMIOJob *job = opaque;
    job->value = (uint32_t)rvg_iosurface_device_mmio_read(job->state->pgiosfc,
                                                          job->offset);
    qatomic_set(&job->completed, true);
    aio_wait_kick();
    return 0;
}

static uint64_t iosfc_read(void *opaque, hwaddr offset, unsigned size)
{
    AppleGFXMMIOState *s = opaque;
    AppleGFXMMIOJob job = {
        .state = s,
        .offset = offset,
        .completed = false,
    };

    /* On a pool thread, as apple-gfx.c's apple_gfx_write says. */
    thread_pool_submit_immediate(s->common.pool, iosfc_do_read, &job, NULL);
    AIO_WAIT_WHILE(NULL, !qatomic_read(&job.completed));

    trace_apple_gfx_mmio_iosfc_read(offset, job.value);
    return job.value;
}

static int iosfc_do_write(void *opaque)
{
    AppleGFXMMIOJob *job = opaque;
    rvg_iosurface_device_mmio_write(job->state->pgiosfc, job->offset,
                                    (uint32_t)job->value);
    qatomic_set(&job->completed, true);
    aio_wait_kick();
    return 0;
}

static void iosfc_write(void *opaque, hwaddr offset, uint64_t val,
                        unsigned size)
{
    AppleGFXMMIOState *s = opaque;
    AppleGFXMMIOJob job = {
        .state = s,
        .offset = offset,
        .value = val,
        .completed = false,
    };

    thread_pool_submit_immediate(s->common.pool, iosfc_do_write, &job, NULL);
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

/* On the IOSurface device's own thread. */
static void apple_gfx_mmio_iosfc_raise_interrupt_cb(void *ctx, uint32_t vector)
{
    AppleGFXMMIOState *s = ctx;
    trace_apple_gfx_iosfc_raise_irq(vector);
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            raise_irq_bh, &s->irq_iosfc);
}

static bool apple_gfx_mmio_create_iosurface_device(AppleGFXMMIOState *s,
                                                   Error **errp)
{
    /* The library copies the ranges. */
    g_autoptr(GArray) memory_ranges = apple_gfx_new_memory_ranges();
    RvgMemoryMap memory_map = {
        .ranges = (const RvgMemoryRange *)memory_ranges->data,
        .range_count = memory_ranges->len,
    };
    RvgIOSurfaceCallbacks callbacks = {
        .struct_size = sizeof(callbacks),
        .ctx = s,
        .raise_interrupt = apple_gfx_mmio_iosfc_raise_interrupt_cb,
    };
    RvgIOSurfaceDeviceConfig config = {
        .struct_size = sizeof(config),
        .memory_map = &memory_map,
        .callbacks = &callbacks,
    };
    RvgStatus status;

    status = rvg_iosurface_device_create(&config, &s->pgiosfc);
    if (status != RVG_OK) {
        error_setg(errp, "apple-gfx-mmio: rvg_iosurface_device_create: %s",
                   rvg_status_message(status));
        return false;
    }
    return true;
}

/*
 * On a library thread (a FIFO's or the device's reporting thread), or inside
 * a call that may call back, such as apple_gfx_mmio_reset's.
 */
static void apple_gfx_mmio_raise_interrupt_cb(void *ctx, uint32_t vector)
{
    AppleGFXMMIOState *s = ctx;
    trace_apple_gfx_raise_irq(vector);
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            raise_irq_bh, &s->irq_gfx);
}

static void apple_gfx_mmio_realize(DeviceState *dev, Error **errp)
{
    AppleGFXMMIOState *s = APPLE_GFX_MMIO(dev);
    RvgHostCallbacks callbacks = {
        .struct_size = sizeof(callbacks),
        .ctx = s,
        .raise_interrupt = apple_gfx_mmio_raise_interrupt_cb,
    };
    RvgDeviceConfig config = {
        .struct_size = sizeof(config),
        /* The IOSurface device made below maps the guest's IOSurfaces. */
        .use_iosurface_mapper = true,
        .process_isolation = true,
        .callbacks = &callbacks,
    };

    if (!apple_gfx_common_realize(&s->common, dev, &config, errp)) {
        return;
    }

    apple_gfx_mmio_create_iosurface_device(s, errp);
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
    /* The guest's display ports, 1 to RVG_MAX_DISPLAY_PORTS. */
    DEFINE_PROP_UINT32("displays", AppleGFXMMIOState, common.num_displays, 1),
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
