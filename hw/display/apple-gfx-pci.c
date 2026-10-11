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
 * Its default option ROM is the reims-vgpu package's EFI driver for the
 * device, which QEMU's build installs with its data files. BAR 2 shows the
 * RAM of the ROM's framebuffer: macOS without a driver of the device's own
 * (installers, recovery) takes a boot framebuffer over only inside a BAR.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "apple-gfx.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(AppleGFXPCIState, APPLE_GFX_PCI)

struct AppleGFXPCIState {
    PCIDevice parent_obj;

    AppleGFXState common;
    bool efi_fb_bar;
    MemoryRegion efi_fb_bar_mr; /* BAR 2 */
    MemoryRegion efi_fb; /* in BAR 2: an alias of the machine's RAM */
};

#define APPLE_GFX_PCI_EFI_FB_BAR_SIZE (64 << 20)
/* The library's page for the EFI registers: APV_PAGE_SHIFT in reims-vgpu */
#ifdef HOST_X86_64
#define APPLE_GFX_PCI_PAGE_SHIFT 12
#else
#define APPLE_GFX_PCI_PAGE_SHIFT 14
#endif

static void apple_gfx_pci_init(Object *obj)
{
    AppleGFXPCIState *s = APPLE_GFX_PCI(obj);

    apple_gfx_common_init(obj, &s->common, TYPE_APPLE_GFX_PCI);
}

static void apple_gfx_pci_finalize(Object *obj)
{
    AppleGFXPCIState *s = APPLE_GFX_PCI(obj);

    apple_gfx_common_finalize(&s->common);
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
 * a call that may call back: an MMIO access, on the vCPU's thread, or
 * apple_gfx_pci_reset's.
 */
static void apple_gfx_pci_raise_interrupt_cb(void *ctx, uint32_t vector)
{
    PCIDevice *dev = ctx;
    AppleGFXPCIInterruptJob *job;

    trace_apple_gfx_raise_irq(vector);
    job = g_new0(AppleGFXPCIInterruptJob, 1);
    job->device = dev;
    job->vector = vector;
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            apple_gfx_pci_raise_interrupt, job);
}

/*
 * After a write to 0x1210, BAR 2 shows the run of RAM at the framebuffer's
 * start, up to 64 MiB: only RAM the guest already reaches there, so not SMRAM
 * or other RAM the flat view hides. Only these writes and a reset change it,
 * so the kernel's draws after EFI goes off still land in RAM.
 */
static void apple_gfx_pci_mmio_written(AppleGFXState *common, hwaddr offset,
                                       uint32_t value)
{
    AppleGFXPCIState *s = container_of(common, AppleGFXPCIState, common);
    MemoryRegionSection sec = {};
    uint32_t start;
    bool ram;

    if (offset != 0x1210) {
        return;
    }
    BQL_LOCK_GUARD(); /* under KVM the access comes without it */
    /* The library's start, not this write's: vCPUs' hooks race to the BQL */
    start = rvg_device_mmio_read(common->pgdev, 0x1210);
    if (start) {
        sec = memory_region_find(get_system_memory(),
                                 (hwaddr)start << APPLE_GFX_PCI_PAGE_SHIFT,
                                 APPLE_GFX_PCI_EFI_FB_BAR_SIZE);
    }
    ram = sec.mr == MACHINE(qdev_get_machine())->ram;
    memory_region_transaction_begin();
    if (ram) {
        memory_region_set_alias_offset(&s->efi_fb, sec.offset_within_region);
        memory_region_set_size(&s->efi_fb, int128_get64(sec.size));
    }
    memory_region_set_enabled(&s->efi_fb, ram);
    memory_region_transaction_commit();
    memory_region_unref(sec.mr);
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
    MemoryRegion *ram = MACHINE(qdev_get_machine())->ram;
    int ret;

    pci_register_bar(dev, RVG_PCI_BAR_MMIO,
                     PCI_BASE_ADDRESS_SPACE_MEMORY, &s->common.iomem_gfx);
    /* A second KVM slot onto a guest_memfd's RAM would be refused */
    if (s->efi_fb_bar && ram && !memory_region_has_guest_memfd(ram)) {
        memory_region_init(&s->efi_fb_bar_mr, OBJECT(dev),
                           "apple-gfx-pci.efi-fb-bar",
                           APPLE_GFX_PCI_EFI_FB_BAR_SIZE);
        memory_region_init_alias(&s->efi_fb, OBJECT(dev),
                                 "apple-gfx-pci.efi-fb", ram, 0,
                                 APPLE_GFX_PCI_EFI_FB_BAR_SIZE);
        memory_region_set_enabled(&s->efi_fb, false);
        memory_region_add_subregion(&s->efi_fb_bar_mr, 0, &s->efi_fb);
        pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY |
                         PCI_BASE_ADDRESS_MEM_PREFETCH, &s->efi_fb_bar_mr);
        s->common.mmio_written = apple_gfx_pci_mmio_written;
    }

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
    if (s->common.mmio_written) {
        memory_region_set_enabled(&s->efi_fb, false);
    }
}

static const Property apple_gfx_pci_properties[] = {
    DEFINE_PROP_ARRAY("display-modes", AppleGFXPCIState,
                      common.num_display_modes, common.display_modes,
                      qdev_prop_apple_gfx_display_mode, AppleGFXDisplayMode),
    /* The guest's display ports, 1 to RVG_MAX_DISPLAY_PORTS. */
    DEFINE_PROP_UINT32("displays", AppleGFXPCIState, common.num_displays, 1),
    /*
     * In window mode, the mode the guest runs first, scaled by the window:
     * "<width>x<height>" (default 1920x1080), at 1x or followed by "@<ppi>",
     * the density the guest is told for it, or "host" for the host screen's
     * own modes at its density.
     */
    DEFINE_PROP_STRING("resolution", AppleGFXPCIState, common.resolution),
    /* BAR 2, the EFI framebuffer's window for guests without a driver */
    DEFINE_PROP_BOOL("efi-framebuffer-bar", AppleGFXPCIState, efi_fb_bar, true),
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
    pci->romfile = "ReimsVgpu.rom";
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
        .instance_finalize = apple_gfx_pci_finalize,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { },
        },
    }
};
DEFINE_TYPES(apple_gfx_pci_types)
