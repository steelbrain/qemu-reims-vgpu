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
 *
 * apple-gfx-iosurface-pci, below, is the IOSurface device, which maps the
 * guest's IOSurfaces, as a PCI device of its own: vmapple's second MMIO
 * region (apple-gfx-mmio.c), for guests that drive both over PCI.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
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
        /*
         * Map the guest's IOSurfaces through the IOSurface device an
         * apple-gfx-iosurface-pci makes, and run a guest task but task 0
         * isolated, as apple-gfx-mmio does, only when the guest defines it
         * while that device has a descriptor table: the setup in which the
         * library writes info replies into guest memory, which in process end
         * it. Device info offers argument buffers, which only isolated tasks
         * serve, under the same rule. The port's mapper driver writes the
         * table's base at its start, before Apple's arm64 GPU driver makes a
         * task. Apple's x86-64 driver never touches the mapper, and references
         * IOSurfaces through task 0's backings, which an isolated task does
         * not serve: its tasks run in process, and so does every task of a VM
         * with no apple-gfx-iosurface-pci. Isolation needs the memory map,
         * which apple_gfx_common_realize always gives.
         */
        .use_iosurface_mapper = true,
        .process_isolation = true,
        .process_isolation_follows_guest = true,
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

/* ------ The IOSurface device over PCI ------ */

OBJECT_DECLARE_SIMPLE_TYPE(AppleGFXIOSurfacePCIState, APPLE_GFX_IOSURFACE_PCI)

/*
 * The IOSurface device's registers, at 0x1000 to 0x1030 of BAR 0, as in
 * vmapple's second MMIO region; one MSI vector, raised once per completed
 * command. The device ID is ours: no Apple device has it, and the guest's
 * mapper driver is matched to it by a personality of the port's.
 */
#define APPLE_GFX_IOSURFACE_PCI_DEVICE_ID 0xeeef
#define APPLE_GFX_IOSURFACE_PCI_MMIO_SIZE 0x10000
#define APPLE_GFX_IOSURFACE_PCI_MSI_VECTORS 1

struct AppleGFXIOSurfacePCIState {
    PCIDevice parent_obj;

    /* The apple-gfx-pci whose guest IOSurfaces this device maps */
    AppleGFXPCIState *gpu;
    MemoryRegion iomem; /* BAR 0 */
    RvgIOSurfaceDevice *pgiosfc;
};

/*
 * The registers are 64 bits wide, and an 8-byte access passes whole: the
 * ring's and the descriptor table's bases are guest physical addresses, which
 * may lie above 4 GiB. A 4-byte access reads the low half of the register at
 * its offset, and writes it zero-extended, as on apple-gfx-mmio. The vCPU
 * calls the library itself: an access waits only on the IOSurface device's
 * thread, whose one callback schedules a BH.
 */
static uint64_t apple_gfx_iosurface_pci_read(void *opaque, hwaddr offset,
                                             unsigned size)
{
    AppleGFXIOSurfacePCIState *s = opaque;
    uint64_t value = rvg_iosurface_device_mmio_read(s->pgiosfc, offset);

    trace_apple_gfx_pci_iosfc_read(offset, value, size);
    return value;
}

static void apple_gfx_iosurface_pci_write(void *opaque, hwaddr offset,
                                          uint64_t val, unsigned size)
{
    AppleGFXIOSurfacePCIState *s = opaque;

    rvg_iosurface_device_mmio_write(s->pgiosfc, offset, val);
    trace_apple_gfx_pci_iosfc_write(offset, val, size);
}

static const MemoryRegionOps apple_gfx_iosurface_pci_ops = {
    .read = apple_gfx_iosurface_pci_read,
    .write = apple_gfx_iosurface_pci_write,
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

/* On the IOSurface device's own thread, once per completed command. */
static void apple_gfx_iosurface_pci_raise_interrupt_cb(void *ctx,
                                                       uint32_t vector)
{
    AppleGFXPCIInterruptJob *job;

    trace_apple_gfx_iosfc_raise_irq(vector);
    job = g_new0(AppleGFXPCIInterruptJob, 1);
    job->device = ctx;
    job->vector = vector;
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            apple_gfx_pci_raise_interrupt, job);
}

static void apple_gfx_iosurface_pci_realize(PCIDevice *dev, Error **errp)
{
    AppleGFXIOSurfacePCIState *s = APPLE_GFX_IOSURFACE_PCI(dev);
    /* The library copies the ranges. */
    g_autoptr(GArray) memory_ranges = NULL;
    RvgMemoryMap memory_map;
    RvgIOSurfaceCallbacks callbacks = {
        .struct_size = sizeof(callbacks),
        .ctx = dev,
        .raise_interrupt = apple_gfx_iosurface_pci_raise_interrupt_cb,
    };
    RvgIOSurfaceDeviceConfig config = {
        .struct_size = sizeof(config),
        .memory_map = &memory_map,
        .callbacks = &callbacks,
    };
    bool ambiguous = false;
    RvgStatus status;
    int ret;

    /*
     * Without a gpu link, the machine's only apple-gfx-pci, made before it.
     * The library pairs the two itself: apple-gfx-pci maps through the
     * process's latest IOSurface device.
     */
    if (!s->gpu) {
        Object *gpu = object_resolve_path_type("", TYPE_APPLE_GFX_PCI,
                                               &ambiguous);

        if (ambiguous) {
            error_setg(errp, "%s: the machine has more than one apple-gfx-pci: "
                       "set gpu to the one whose IOSurfaces it maps",
                       TYPE_APPLE_GFX_IOSURFACE_PCI);
            return;
        }
        if (!gpu) {
            error_setg(errp, "%s: the machine has no apple-gfx-pci before it "
                       "whose IOSurfaces it could map",
                       TYPE_APPLE_GFX_IOSURFACE_PCI);
            return;
        }
    }
    /*
     * apple-gfx-pci maps through the process's latest IOSurface device, so a
     * second one would take every gpu's IOSurfaces.
     */
    ambiguous = false;
    object_resolve_path_type("", TYPE_APPLE_GFX_IOSURFACE_PCI, &ambiguous);
    if (ambiguous) {
        error_setg(errp, "%s: only one per machine",
                   TYPE_APPLE_GFX_IOSURFACE_PCI);
        return;
    }

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->iomem);
    ret = msi_init(dev, 0x0 /* config offset; 0 = find space */,
                   APPLE_GFX_IOSURFACE_PCI_MSI_VECTORS, true /* msi64bit */,
                   false /* msi_per_vector_mask */, errp);
    if (ret != 0) {
        return;
    }

    /*
     * The guest's RAM, as the gpu's device has it. On Linux the library maps
     * guest pages through the files the gpu's device names for that RAM.
     */
    memory_ranges = apple_gfx_new_memory_ranges(NULL);
    memory_map = (RvgMemoryMap) {
        .ranges = (const RvgMemoryRange *)memory_ranges->data,
        .range_count = memory_ranges->len,
    };
    status = rvg_iosurface_device_create(&config, &s->pgiosfc);
    if (status != RVG_OK) {
        error_setg(errp, "%s: rvg_iosurface_device_create: %s",
                   TYPE_APPLE_GFX_IOSURFACE_PCI, rvg_status_message(status));
        msi_uninit(dev);
    }
}

/*
 * Return the IOSurface device's registers to 0 through the library's MMIO
 * writes, as a guest would, so a guest rebooted into Apple's x86-64 driver
 * finds no descriptor table its last driver left: the ring base unmaps the
 * ring, the descriptor enable and the table base tear every surface down and
 * drop the table. The library ignores a ring size of 0, so that register
 * keeps its size, which maps nothing until a base and a size are written
 * again; the write counter's doorbell runs nothing without a ring.
 */
static void apple_gfx_iosurface_pci_reset(Object *obj, ResetType type)
{
    AppleGFXIOSurfacePCIState *s = APPLE_GFX_IOSURFACE_PCI(obj);
    static const hwaddr regs[] = {
        0x1000, /* the ring base */
        0x1030, /* the descriptor enable */
        0x1010, /* the descriptor table base */
        0x1008, /* the ring size */
        0x1020, /* the read counter */
        0x1018, /* the write counter */
    };

    if (!s->pgiosfc) {
        return;
    }
    for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
        rvg_iosurface_device_mmio_write(s->pgiosfc, regs[i], 0);
    }
}

static void apple_gfx_iosurface_pci_init(Object *obj)
{
    AppleGFXIOSurfacePCIState *s = APPLE_GFX_IOSURFACE_PCI(obj);

    memory_region_init_io(&s->iomem, obj, &apple_gfx_iosurface_pci_ops, s,
                          TYPE_APPLE_GFX_IOSURFACE_PCI,
                          APPLE_GFX_IOSURFACE_PCI_MMIO_SIZE);
}

static const Property apple_gfx_iosurface_pci_properties[] = {
    /*
     * The apple-gfx-pci it maps IOSurfaces for, made before it; unset, the
     * machine's only one. Adding this device is what lets a guest's tasks run
     * isolated and device info offer argument buffers: apple-gfx-pci has no
     * option for either.
     */
    DEFINE_PROP_LINK("gpu", AppleGFXIOSurfacePCIState, gpu, TYPE_APPLE_GFX_PCI,
                     AppleGFXPCIState *),
};

static void apple_gfx_iosurface_pci_class_init(ObjectClass *klass,
                                               const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pci = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "macOS Paravirtualized Graphics IOSurface Mapper";
    rc->phases.hold = apple_gfx_iosurface_pci_reset;
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);

    pci->vendor_id = RVG_PCI_VENDOR_ID;
    pci->device_id = APPLE_GFX_IOSURFACE_PCI_DEVICE_ID;
    pci->class_id = PCI_CLASS_SYSTEM_OTHER;
    pci->realize = apple_gfx_iosurface_pci_realize;

    device_class_set_props(dc, apple_gfx_iosurface_pci_properties);
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
    },
    {
        .name          = TYPE_APPLE_GFX_IOSURFACE_PCI,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(AppleGFXIOSurfacePCIState),
        .class_init    = apple_gfx_iosurface_pci_class_init,
        .instance_init = apple_gfx_iosurface_pci_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { },
        },
    },
};
DEFINE_TYPES(apple_gfx_pci_types)
