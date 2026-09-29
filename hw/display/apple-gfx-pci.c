/*
 * QEMU Apple paravirtualized graphics device, PCI variant
 *
 * Copyright © 2023-2024 Phil Dennis-Jordan
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * macOS guests drive a paravirtualized GPU, the device of Apple's
 * ParavirtualizedGraphics.framework, which implements 3d graphics passthrough
 * to the host as well as a proprietary guest communication channel. This
 * device model drives reims-vgpu, a library that implements the host side of
 * that device, from within QEMU as a PCI device aimed primarily at x86-64
 * macOS VMs.
 *
 * The device has no default option ROM: pass one with romfile= where the
 * guest's firmware needs its EFI driver.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "apple-gfx.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(AppleGFXPCIState, APPLE_GFX_PCI)

struct AppleGFXPCIState {
    PCIDevice parent_obj;

    AppleGFXState common;
};

static void apple_gfx_pci_init(Object *obj)
{
    AppleGFXPCIState *s = APPLE_GFX_PCI(obj);

    apple_gfx_common_init(obj, &s->common, TYPE_APPLE_GFX_PCI);
}

typedef struct AppleGFXPCIInterruptJob {
    PCIDevice *device;
    uint32_t vector;
} AppleGFXPCIInterruptJob;

static void apple_gfx_pci_raise_interrupt(void *opaque)
{
    AppleGFXPCIInterruptJob *job = opaque;

    if (msi_enabled(job->device)) {
        msi_notify(job->device, job->vector);
    }
    g_free(job);
}

/*
 * On a library thread (a FIFO's or the device's reporting thread), or inside
 * a call that may call back, such as apple_gfx_pci_reset's.
 */
static void apple_gfx_pci_raise_interrupt_cb(void *ctx, uint32_t vector)
{
    PCIDevice *dev = ctx;
    AppleGFXPCIInterruptJob *job;

    trace_apple_gfx_raise_irq(vector);
    job = g_malloc0(sizeof(*job));
    job->device = dev;
    job->vector = vector;
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            apple_gfx_pci_raise_interrupt, job);
}

static void apple_gfx_pci_realize(PCIDevice *dev, Error **errp)
{
    AppleGFXPCIState *s = APPLE_GFX_PCI(dev);
    RvgHostCallbacks callbacks = {
        .struct_size = sizeof(callbacks),
        .ctx = dev,
        .raise_interrupt = apple_gfx_pci_raise_interrupt_cb,
    };
    RvgDeviceConfig config = {
        .struct_size = sizeof(config),
        .callbacks = &callbacks,
    };
    int ret;

    pci_register_bar(dev, RVG_PCI_BAR_MMIO,
                     PCI_BASE_ADDRESS_SPACE_MEMORY, &s->common.iomem_gfx);

    ret = msi_init(dev, 0x0 /* config offset; 0 = find space */,
                   RVG_PCI_MSI_VECTORS, true /* msi64bit */,
                   false /* msi_per_vector_mask */, errp);
    if (ret != 0) {
        return;
    }

    apple_gfx_common_realize(&s->common, DEVICE(dev), &config, errp);
}

static void apple_gfx_pci_reset(Object *obj, ResetType type)
{
    AppleGFXPCIState *s = APPLE_GFX_PCI(obj);
    rvg_device_reset(s->common.pgdev);
}

static const Property apple_gfx_pci_properties[] = {
    DEFINE_PROP_ARRAY("display-modes", AppleGFXPCIState,
                      common.num_display_modes, common.display_modes,
                      qdev_prop_apple_gfx_display_mode, AppleGFXDisplayMode),
};

static void apple_gfx_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pci = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = apple_gfx_pci_reset;
    dc->desc = "macOS Paravirtualized Graphics PCI Display Controller";
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);

    pci->vendor_id = RVG_PCI_VENDOR_ID;
    pci->device_id = RVG_PCI_DEVICE_ID;
    pci->class_id = PCI_CLASS_DISPLAY_OTHER;
    pci->realize = apple_gfx_pci_realize;

    device_class_set_props(dc, apple_gfx_pci_properties);
}

static const TypeInfo apple_gfx_pci_types[] = {
    {
        .name          = TYPE_APPLE_GFX_PCI,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(AppleGFXPCIState),
        .class_init    = apple_gfx_pci_class_init,
        .instance_init = apple_gfx_pci_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { },
        },
    }
};
DEFINE_TYPES(apple_gfx_pci_types)
