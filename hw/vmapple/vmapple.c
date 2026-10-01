/*
 * VMApple machine emulation
 *
 * Copyright © 2023 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * VMApple is the device model that the macOS built-in hypervisor called
 * "Virtualization.framework" exposes to Apple Silicon macOS guests. The
 * machine model in this file implements the same device model in QEMU, but
 * does not use any code from Virtualization.Framework.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/cacheflush.h"
#include "qemu/cutils.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/help-texts.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/units.h"
#include "monitor/qdev.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/usb/usb.h"
#include "hw/arm/boot.h"
#include "hw/char/pl011.h"
#include "hw/intc/arm_gic.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/misc/pvpanic.h"
#include "hw/pci-host/gpex.h"
#include "hw/usb/hcd-xhci-pci.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/vmapple/vmapple.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qapi/qapi-visit-common.h"
#include "qobject/qlist.h"
#include "standard-headers/linux/input.h"
#include "system/hvf.h"
#include "system/hvf_int.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "target/arm/gtimer.h"
#include "target/arm/cpu.h"
#include "target/arm/hvf_arm.h"

struct VMAppleMachineState {
    MachineState parent;

    Notifier machine_done;
    struct arm_boot_info bootinfo;
    const MemMapEntry *memmap;
    const int *irqmap;
    DeviceState *gic;
    DeviceState *cfg;
    DeviceState *pvpanic;
    Notifier powerdown_notifier;
    PCIBus *bus;
    MemoryRegion fw_mr;
    MemoryRegion ecam_alias;
    uint64_t uuid;
    char *tablet;
};

#define TYPE_VMAPPLE_MACHINE   MACHINE_TYPE_NAME("vmapple")
OBJECT_DECLARE_SIMPLE_TYPE(VMAppleMachineState, VMAPPLE_MACHINE)

/* Number of external interrupt lines to configure the GIC with */
#define NUM_IRQS 256

enum {
    VMAPPLE_FIRMWARE,
    VMAPPLE_CONFIG,
    VMAPPLE_MEM,
    VMAPPLE_GIC_DIST,
    VMAPPLE_GIC_REDIST,
    VMAPPLE_GICV2M,
    VMAPPLE_UART,
    VMAPPLE_RTC,
    VMAPPLE_PCIE,
    VMAPPLE_PCIE_MMIO,
    VMAPPLE_PCIE_ECAM,
    VMAPPLE_GPIO,
    VMAPPLE_PVPANIC,
    VMAPPLE_APV_GFX,
    VMAPPLE_APV_IOSFC,
    VMAPPLE_AES_1,
    VMAPPLE_AES_2,
    VMAPPLE_BDOOR,
    VMAPPLE_MEMMAP_LAST,
};

static const MemMapEntry memmap[] = {
    [VMAPPLE_FIRMWARE] =           { 0x00100000, 0x00100000 },
    [VMAPPLE_CONFIG] =             { 0x00400000, 0x00010000 },

    [VMAPPLE_GIC_DIST] =           { 0x10000000, 0x00010000 },
    [VMAPPLE_GIC_REDIST] =         { 0x10010000, 0x00400000 },
    [VMAPPLE_GICV2M] =             { 0x1FFF0000, 0x00010000 },

    [VMAPPLE_UART] =               { 0x20010000, 0x00010000 },
    [VMAPPLE_RTC] =                { 0x20050000, 0x00001000 },
    [VMAPPLE_GPIO] =               { 0x20060000, 0x00001000 },
    [VMAPPLE_PVPANIC] =            { 0x20070000, 0x00000002 },
    [VMAPPLE_BDOOR] =              { 0x30000000, 0x00200000 },
    [VMAPPLE_APV_GFX] =            { 0x30200000, 0x00010000 },
    [VMAPPLE_APV_IOSFC] =          { 0x30210000, 0x00010000 },
    [VMAPPLE_AES_1] =              { 0x30220000, 0x00004000 },
    [VMAPPLE_AES_2] =              { 0x30230000, 0x00004000 },
    [VMAPPLE_PCIE_ECAM] =          { 0x40000000, 0x10000000 },
    [VMAPPLE_PCIE_MMIO] =          { 0x50000000, 0x1fff0000 },

    /* Actual RAM size depends on configuration */
    [VMAPPLE_MEM] =                { 0x70000000ULL, GiB},
};

static const int irqmap[] = {
    [VMAPPLE_UART] = 1,
    [VMAPPLE_RTC] = 2,
    [VMAPPLE_GPIO] = 0x5,
    [VMAPPLE_APV_IOSFC] = 0x10,
    [VMAPPLE_APV_GFX] = 0x11,
    [VMAPPLE_AES_1] = 0x12,
    [VMAPPLE_PCIE] = 0x20,
    [VMAPPLE_GICV2M] = 0x80
};

#define GPEX_NUM_IRQS 16

static void create_bdif(VMAppleMachineState *vms, MemoryRegion *mem)
{
    DeviceState *bdif;
    SysBusDevice *bdif_sb;
    DriveInfo *di_aux = drive_get(IF_PFLASH, 0, 0);
    DriveInfo *di_root = drive_get(IF_PFLASH, 0, 1);

    if (!di_aux) {
        error_report("No AUX device. Please specify one as pflash drive.");
        exit(1);
    }

    if (!di_root) {
        /* Fall back to the first IF_VIRTIO device as root device */
        di_root = drive_get(IF_VIRTIO, 0, 0);
    }

    if (!di_root) {
        error_report("No root device. Please specify one as virtio drive.");
        exit(1);
    }

    /* PV backdoor device */
    bdif = qdev_new(TYPE_VMAPPLE_BDIF);
    bdif_sb = SYS_BUS_DEVICE(bdif);
    sysbus_mmio_map(bdif_sb, 0, vms->memmap[VMAPPLE_BDOOR].base);

    qdev_prop_set_drive(DEVICE(bdif), "aux", blk_by_legacy_dinfo(di_aux));
    qdev_prop_set_drive(DEVICE(bdif), "root", blk_by_legacy_dinfo(di_root));

    sysbus_realize_and_unref(bdif_sb, &error_fatal);
}

static void create_pvpanic(VMAppleMachineState *vms, MemoryRegion *mem)
{
    SysBusDevice *pvpanic;

    vms->pvpanic = qdev_new(TYPE_PVPANIC_MMIO_DEVICE);
    pvpanic = SYS_BUS_DEVICE(vms->pvpanic);
    sysbus_mmio_map(pvpanic, 0, vms->memmap[VMAPPLE_PVPANIC].base);

    sysbus_realize_and_unref(pvpanic, &error_fatal);
}

static bool create_cfg(VMAppleMachineState *vms, MemoryRegion *mem,
                       Error **errp)
{
    ERRP_GUARD();
    SysBusDevice *cfg;
    MachineState *machine = MACHINE(vms);
    uint32_t rnd = 1;

    vms->cfg = qdev_new(TYPE_VMAPPLE_CFG);
    cfg = SYS_BUS_DEVICE(vms->cfg);
    sysbus_mmio_map(cfg, 0, vms->memmap[VMAPPLE_CONFIG].base);

    qemu_guest_getrandom_nofail(&rnd, sizeof(rnd));

    qdev_prop_set_uint32(vms->cfg, "nr-cpus", machine->smp.cpus);
    qdev_prop_set_uint64(vms->cfg, "ecid", vms->uuid);
    qdev_prop_set_uint64(vms->cfg, "ram-size", machine->ram_size);
    qdev_prop_set_uint32(vms->cfg, "rnd", rnd);

    if (!sysbus_realize_and_unref(cfg, errp)) {
        error_prepend(errp, "Error creating vmapple cfg device: ");
        return false;
    }

    return true;
}

static void create_gfx(VMAppleMachineState *vms, MemoryRegion *mem)
{
    int irq_gfx = vms->irqmap[VMAPPLE_APV_GFX];
    int irq_iosfc = vms->irqmap[VMAPPLE_APV_IOSFC];
    SysBusDevice *gfx;

    gfx = SYS_BUS_DEVICE(qdev_new("apple-gfx-mmio"));
    sysbus_mmio_map(gfx, 0, vms->memmap[VMAPPLE_APV_GFX].base);
    sysbus_mmio_map(gfx, 1, vms->memmap[VMAPPLE_APV_IOSFC].base);
    sysbus_connect_irq(gfx, 0, qdev_get_gpio_in(vms->gic, irq_gfx));
    sysbus_connect_irq(gfx, 1, qdev_get_gpio_in(vms->gic, irq_iosfc));
    sysbus_realize_and_unref(gfx, &error_fatal);
}

static void create_aes(VMAppleMachineState *vms, MemoryRegion *mem)
{
    int irq = vms->irqmap[VMAPPLE_AES_1];
    SysBusDevice *aes;

    aes = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_AES));
    sysbus_mmio_map(aes, 0, vms->memmap[VMAPPLE_AES_1].base);
    sysbus_mmio_map(aes, 1, vms->memmap[VMAPPLE_AES_2].base);
    sysbus_connect_irq(aes, 0, qdev_get_gpio_in(vms->gic, irq));
    sysbus_realize_and_unref(aes, &error_fatal);
}

static int arm_gic_ppi_index(int cpu_nr, int ppi_index)
{
    return NUM_IRQS + cpu_nr * GIC_INTERNAL + ppi_index;
}

static void create_gic(VMAppleMachineState *vms, MemoryRegion *mem)
{
    MachineState *ms = MACHINE(vms);
    /* We create a standalone GIC */
    SysBusDevice *gicbusdev;
    QList *redist_region_count;
    int i;
    unsigned int smp_cpus = ms->smp.cpus;

    vms->gic = qdev_new(gicv3_class_name());
    qdev_prop_set_uint32(vms->gic, "revision", 3);
    qdev_prop_set_uint32(vms->gic, "num-cpu", smp_cpus);
    /*
     * Note that the num-irq property counts both internal and external
     * interrupts; there are always 32 of the former (mandated by GIC spec).
     */
    qdev_prop_set_uint32(vms->gic, "num-irq", NUM_IRQS + 32);

    uint32_t redist0_capacity =
                vms->memmap[VMAPPLE_GIC_REDIST].size / GICV3_REDIST_SIZE;
    uint32_t redist0_count = MIN(smp_cpus, redist0_capacity);

    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, redist0_count);
    qdev_prop_set_array(vms->gic, "redist-region-count", redist_region_count);

    gicbusdev = SYS_BUS_DEVICE(vms->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, vms->memmap[VMAPPLE_GIC_DIST].base);
    sysbus_mmio_map(gicbusdev, 1, vms->memmap[VMAPPLE_GIC_REDIST].base);

    /*
     * Wire the outputs from each CPU's generic timer and the GICv3
     * maintenance interrupt signal to the appropriate GIC PPI inputs,
     * and the GIC's IRQ/FIQ/VIRQ/VFIQ interrupt outputs to the CPU's inputs.
     */
    for (i = 0; i < smp_cpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));

        /* Map the virt timer to PPI 27 */
        qdev_connect_gpio_out(cpudev, GTIMER_VIRT,
                              qdev_get_gpio_in(vms->gic,
                                               arm_gic_ppi_index(i, 27)));

        /* Map the GIC IRQ and FIQ lines to CPU */
        sysbus_connect_irq(gicbusdev, i, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
    }
}

#define NUM_GICV2M_SPIS       128

static void create_gicv2m(VMAppleMachineState *vms)
{
    int i;
    int irq = vms->irqmap[VMAPPLE_GICV2M];
    DeviceState *dev;

    dev = qdev_new("arm-gicv2m");
    qdev_prop_set_uint32(dev, "base-spi", irq);
    qdev_prop_set_uint32(dev, "num-spi", NUM_GICV2M_SPIS);
    qdev_prop_set_uint32(dev, "macos-compat", 1);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, vms->memmap[VMAPPLE_GICV2M].base);

    for (i = 0; i < NUM_GICV2M_SPIS; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i,
                           qdev_get_gpio_in(vms->gic, irq + i));
    }
}

static void create_uart(const VMAppleMachineState *vms, int uart,
                        MemoryRegion *mem, Chardev *chr)
{
    hwaddr base = vms->memmap[uart].base;
    int irq = vms->irqmap[uart];
    DeviceState *dev = qdev_new(TYPE_PL011);
    SysBusDevice *s = SYS_BUS_DEVICE(dev);

    qdev_prop_set_chr(dev, "chardev", chr);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(mem, base,
                                sysbus_mmio_get_region(s, 0));
    sysbus_connect_irq(s, 0, qdev_get_gpio_in(vms->gic, irq));
}

static void create_rtc(const VMAppleMachineState *vms)
{
    hwaddr base = vms->memmap[VMAPPLE_RTC].base;
    int irq = vms->irqmap[VMAPPLE_RTC];

    sysbus_create_simple("pl031", base, qdev_get_gpio_in(vms->gic, irq));
}

static DeviceState *gpio_key_dev;
static void vmapple_powerdown_req(Notifier *n, void *opaque)
{
    /* use gpio Pin 3 for power button event */
    qemu_set_irq(qdev_get_gpio_in(gpio_key_dev, 0), 1);
}

static void create_gpio_devices(const VMAppleMachineState *vms, int gpio,
                                MemoryRegion *mem)
{
    DeviceState *pl061_dev;
    hwaddr base = vms->memmap[gpio].base;
    int irq = vms->irqmap[gpio];
    SysBusDevice *s;

    pl061_dev = qdev_new("pl061");
    /* Pull lines down to 0 if not driven by the PL061 */
    qdev_prop_set_uint8(pl061_dev, "pullups", 0);
    qdev_prop_set_uint8(pl061_dev, "pulldowns", 0xff);
    s = SYS_BUS_DEVICE(pl061_dev);
    sysbus_realize_and_unref(s, &error_fatal);
    memory_region_add_subregion(mem, base, sysbus_mmio_get_region(s, 0));
    sysbus_connect_irq(s, 0, qdev_get_gpio_in(vms->gic, irq));
    gpio_key_dev = sysbus_create_simple("gpio-key", -1,
                                        qdev_get_gpio_in(pl061_dev, 3));
}

static void vmapple_firmware_init(VMAppleMachineState *vms,
                                  MemoryRegion *sysmem)
{
    hwaddr size = vms->memmap[VMAPPLE_FIRMWARE].size;
    hwaddr base = vms->memmap[VMAPPLE_FIRMWARE].base;
    const char *bios_name;
    int image_size;
    char *fname;

    bios_name = MACHINE(vms)->firmware;
    if (!bios_name) {
        error_report("No firmware specified");
        exit(1);
    }

    fname = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (!fname) {
        error_report("Could not find ROM image '%s'", bios_name);
        exit(1);
    }

    memory_region_init_ram(&vms->fw_mr, NULL, "firmware", size, &error_fatal);
    image_size = load_image_mr(fname, &vms->fw_mr);

    g_free(fname);
    if (image_size < 0) {
        error_report("Could not load ROM image '%s'", bios_name);
        exit(1);
    }

    memory_region_add_subregion(get_system_memory(), base, &vms->fw_mr);
}

static void create_pcie(VMAppleMachineState *vms)
{
    hwaddr base_mmio = vms->memmap[VMAPPLE_PCIE_MMIO].base;
    hwaddr size_mmio = vms->memmap[VMAPPLE_PCIE_MMIO].size;
    hwaddr base_ecam = vms->memmap[VMAPPLE_PCIE_ECAM].base;
    hwaddr size_ecam = vms->memmap[VMAPPLE_PCIE_ECAM].size;
    int irq = vms->irqmap[VMAPPLE_PCIE];
    MemoryRegion *mmio_alias;
    MemoryRegion *mmio_reg;
    MemoryRegion *ecam_reg;
    DeviceState *dev;
    int i;
    PCIHostState *pci;
    DeviceState *usb_controller;
    USBBus *usb_bus;

    dev = qdev_new(TYPE_GPEX_HOST);
    qdev_prop_set_uint32(dev, "num-irqs", GPEX_NUM_IRQS);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* Map only the first size_ecam bytes of ECAM space */
    ecam_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0);
    memory_region_init_alias(&vms->ecam_alias, OBJECT(dev), "pcie-ecam",
                             ecam_reg, 0, size_ecam);
    memory_region_add_subregion(get_system_memory(), base_ecam,
                                &vms->ecam_alias);

    /*
     * Map the MMIO window from [0x50000000-0x7fff0000] in PCI space into
     * system address space at [0x50000000-0x7fff0000].
     */
    mmio_alias = g_new0(MemoryRegion, 1);
    mmio_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 1);
    memory_region_init_alias(mmio_alias, OBJECT(dev), "pcie-mmio",
                             mmio_reg, base_mmio, size_mmio);
    memory_region_add_subregion(get_system_memory(), base_mmio, mmio_alias);

    for (i = 0; i < GPEX_NUM_IRQS; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i,
                           qdev_get_gpio_in(vms->gic, irq + i));
        gpex_set_irq_num(GPEX_HOST(dev), i, irq + i);
    }

    pci = PCI_HOST_BRIDGE(dev);
    vms->bus = pci->bus;
    g_assert(vms->bus);

    while ((dev = qemu_create_nic_device("virtio-net-pci", true, NULL))) {
        qdev_realize_and_unref(dev, BUS(vms->bus), &error_fatal);
    }

    if (defaults_enabled()) {
        usb_controller = qdev_new(TYPE_QEMU_XHCI);
        qdev_realize_and_unref(usb_controller, BUS(pci->bus), &error_fatal);

        usb_bus = USB_BUS(object_resolve_type_unambiguous(TYPE_USB_BUS,
                                                          &error_fatal));
        usb_create_simple(usb_bus, "usb-kbd");
        /* The user names the tablet, which may not realize on its own. */
        usb_realize_and_unref(USB_DEVICE(qdev_new(vms->tablet)), usb_bus,
                              &error_fatal);
    }
}

/*
 * macOS 14 and later need two changes to the kernel's memory to boot here,
 * made when iBoot enters the kernel. From reset, RAM is not executable: each
 * page iBoot runs becomes executable on its first instruction fetch, until a
 * fetch lands in the kernel's memory with x0 at the boot arguments. Only the
 * boot CPU runs then. The changes leave macOS 13 as it was.
 */

/* A host pointer to [pa, pa + len), if guest RAM holds all of it. */
static uint8_t *vmapple_ram(VMAppleMachineState *vms, uint64_t pa,
                            uint64_t len)
{
    MachineState *ms = MACHINE(vms);
    uint64_t off = pa - vms->memmap[VMAPPLE_MEM].base;

    if (pa < vms->memmap[VMAPPLE_MEM].base || off > ms->ram_size ||
        len > ms->ram_size - off) {
        return NULL;
    }
    return (uint8_t *)memory_region_get_ram_ptr(ms->ram) + off;
}

/*
 * Hypervisor.framework fails the kernel's pointer authentication hypercalls,
 * and from macOS 14 on the kernel spins on each failure with
 * "hvc #0; cbnz x0, .". Take out the spins: the kernel then runs on the
 * firmware's keys, as macOS 13's does.
 */
static void vmapple_skip_pac_waits(uint8_t *mem, uint64_t size)
{
    for (uint64_t off = 4; off + 4 <= size; off += 4) {
        if (ldl_le_p(mem + off) == 0xb5000000 &&      /* cbnz x0, . */
            ldl_le_p(mem + off - 4) == 0xd4000002) {  /* hvc #0 */
            stl_le_p(mem + off, 0xd503201f);          /* nop */
            flush_idcache_range((uintptr_t)mem + off, (uintptr_t)mem + off,
                                4);
        }
    }
}

/*
 * A device tree node: a property count, a child count, its properties, then
 * its children. A property is a 32-byte name, a 32-bit length whose top bit
 * is a flag, and its value, padded to 4 bytes.
 */
static const struct QEMU_PACKED {
    uint32_t props, children;
    struct QEMU_PACKED {
        char name[32];
        uint32_t len;
        char value[12];
    } name;
    struct QEMU_PACKED {
        char name[32];
        uint32_t len, value;
    } no_sep;
} vmapple_filesystems = {
    const_le32(2), 0,
    { "name", const_le32(12), "filesystems" },
    { "DoNotUseSEP", const_le32(4), const_le32(1) },
};

/*
 * The machine has no Secure Enclave, and from macOS 15 on APFS panics on the
 * unencrypted data volume unless the device tree has /filesystems/DoNotUseSEP.
 * If the tree has no /filesystems, append that node as the root's last child,
 * in the zeroes that pad the tree to its last 16 KiB page, and cover it with
 * the tree's length in the boot arguments.
 */
static void vmapple_no_sep(VMAppleMachineState *vms, uint8_t *args)
{
    /* deviceTreeP - virtBase + physBase, and deviceTreeLength */
    uint64_t pa = ldq_le_p(args + 0x60) - ldq_le_p(args + 8) +
                  ldq_le_p(args + 16);
    uint64_t len = ldl_le_p(args + 0x68), end = 0, nodes, props;
    uint64_t room = ROUND_UP(pa + len, 16 * KiB) - pa;
    uint64_t size = sizeof(vmapple_filesystems);
    uint8_t *dt = vmapple_ram(vms, pa, room);

    if (!dt) {
        return;
    }
    /* The tree's end: its nodes come in order, each before its children */
    for (nodes = 1; nodes; nodes--) {
        if (end + 8 > len) {
            return;
        }
        props = ldl_le_p(dt + end);
        nodes += ldl_le_p(dt + end + 4);
        for (end += 8; props; props--) {
            if (end + 36 > len) {
                return;
            }
            end += 36 + ROUND_UP(ldl_le_p(dt + end + 32) & INT32_MAX, 4);
        }
    }
    if (end + size > room || !buffer_is_zero(dt + end, size) ||
        memmem(dt, end, &vmapple_filesystems.name,
               sizeof(vmapple_filesystems.name))) {
        return;
    }
    memcpy(dt + end, &vmapple_filesystems, size);
    stl_le_p(dt + 4, ldl_le_p(dt + 4) + 1);
    stl_le_p(args + 0x68, MAX(len, end + size));
}

static bool vmapple_exec_fault(CPUState *cpu, hwaddr ipa)
{
    VMAppleMachineState *vms = VMAPPLE_MACHINE(qdev_get_machine());
    uint64_t x0, base = 0, top = 0;
    uint8_t *args, *kernel;

    if (!vmapple_ram(vms, ipa, 1)) {
        return false;
    }
    /* XNU's boot_args, version 2: physBase and topOfKernelData */
    assert_hvf_ok(hv_vcpu_get_reg(cpu->accel->fd, HV_REG_X0, &x0));
    args = vmapple_ram(vms, x0, 0x70);
    if (args && lduw_le_p(args + 2) == 2) {
        base = ldq_le_p(args + 16);
        top = ldq_le_p(args + 32);
    }
    kernel = vmapple_ram(vms, base, top - base);
    if (ipa < base || ipa >= top || !kernel) {
        /* Still in iBoot */
        assert_hvf_ok(hv_vm_protect(ipa & qemu_real_host_page_mask(),
                                    qemu_real_host_page_size(),
                                    HV_MEMORY_READ | HV_MEMORY_WRITE |
                                    HV_MEMORY_EXEC));
        return true;
    }
    vmapple_no_sep(vms, args);
    vmapple_skip_pac_waits(kernel, top - base);
    hvf_arm_exec_fault = NULL;
    assert_hvf_ok(hv_vm_protect(vms->memmap[VMAPPLE_MEM].base,
                                MACHINE(vms)->ram_size,
                                HV_MEMORY_READ | HV_MEMORY_WRITE |
                                HV_MEMORY_EXEC));
    return true;
}

static void vmapple_reset(void *opaque)
{
    VMAppleMachineState *vms = opaque;
    hwaddr base = vms->memmap[VMAPPLE_FIRMWARE].base;

    cpu_set_pc(first_cpu, base);
    hvf_arm_exec_fault = vmapple_exec_fault;
    assert_hvf_ok(hv_vm_protect(vms->memmap[VMAPPLE_MEM].base,
                                MACHINE(vms)->ram_size,
                                HV_MEMORY_READ | HV_MEMORY_WRITE));
}

static void mach_vmapple_init(MachineState *machine)
{
    VMAppleMachineState *vms = VMAPPLE_MACHINE(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    const CPUArchIdList *possible_cpus;
    MemoryRegion *sysmem = get_system_memory();
    int n;
    unsigned int smp_cpus = machine->smp.cpus;
    unsigned int max_cpus = machine->smp.max_cpus;

    vms->memmap = memmap;
    machine->usb = true;

    possible_cpus = mc->possible_cpu_arch_ids(machine);
    assert(possible_cpus->len == max_cpus);
    for (n = 0; n < possible_cpus->len; n++) {
        Object *cpu;
        CPUState *cs;

        if (n >= smp_cpus) {
            break;
        }

        cpu = object_new(possible_cpus->cpus[n].type);
        object_property_set_int(cpu, "mp-affinity",
                                possible_cpus->cpus[n].arch_id, &error_fatal);

        cs = CPU(cpu);
        cs->cpu_index = n;

        numa_cpu_pre_plug(&possible_cpus->cpus[cs->cpu_index], DEVICE(cpu),
                          &error_fatal);

        if (object_property_find(cpu, "has_el3")) {
            object_property_set_bool(cpu, "has_el3", false, &error_fatal);
        }
        if (object_property_find(cpu, "has_el2")) {
            object_property_set_bool(cpu, "has_el2", false, &error_fatal);
        }
        object_property_set_int(cpu, "psci-conduit", QEMU_PSCI_CONDUIT_HVC,
                                &error_fatal);

        /*
         * macOS asks the hypervisor to program its pointer authentication
         * keys, through the paravirtualized calls SMCCC 0xC1000000 to
         * 0xC1000006. Hypervisor.framework answers them itself, without an
         * exit to QEMU, and programs no key: the guest runs on the keys
         * each CPU holds. The boot firmware sets them on the boot CPU
         * only, so start each CPU the guest powers on with the keys of
         * the CPU that powers it on, and all CPUs share them.
         */
        object_property_set_bool(cpu, "cpu-on-copies-pauth-keys", true,
                                 &error_fatal);

        /* Secondary CPUs start in PSCI powered-down state */
        if (n > 0) {
            object_property_set_bool(cpu, "start-powered-off", true,
                                     &error_fatal);
        }

        object_property_set_link(cpu, "memory", OBJECT(sysmem), &error_abort);
        qdev_realize(DEVICE(cpu), NULL, &error_fatal);
        object_unref(cpu);
    }

    memory_region_add_subregion(sysmem, vms->memmap[VMAPPLE_MEM].base,
                                machine->ram);

    create_gic(vms, sysmem);
    create_gicv2m(vms);
    create_bdif(vms, sysmem);
    create_pvpanic(vms, sysmem);
    create_aes(vms, sysmem);
    create_gfx(vms, sysmem);
    create_uart(vms, VMAPPLE_UART, sysmem, serial_hd(0));
    create_rtc(vms);
    create_pcie(vms);

    create_gpio_devices(vms, VMAPPLE_GPIO, sysmem);

    vmapple_firmware_init(vms, sysmem);
    create_cfg(vms, sysmem, &error_fatal);

    /* connect powerdown request */
    vms->powerdown_notifier.notify = vmapple_powerdown_req;
    qemu_register_powerdown_notifier(&vms->powerdown_notifier);

    vms->bootinfo.ram_size = machine->ram_size;
    vms->bootinfo.board_id = -1;
    vms->bootinfo.loader_start = vms->memmap[VMAPPLE_MEM].base;
    vms->bootinfo.skip_dtb_autoload = true;
    vms->bootinfo.firmware_loaded = true;
    arm_load_kernel(ARM_CPU(first_cpu), machine, &vms->bootinfo);

    qemu_register_reset(vmapple_reset, vms);
}

static CpuInstanceProperties
vmapple_cpu_index_to_props(MachineState *ms, unsigned cpu_index)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(ms);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}


static int64_t vmapple_get_default_cpu_node_id(const MachineState *ms, int idx)
{
    return idx % ms->numa_state->num_nodes;
}

static const CPUArchIdList *vmapple_possible_cpu_arch_ids(MachineState *ms)
{
    int n;
    unsigned int max_cpus = ms->smp.max_cpus;

    if (ms->possible_cpus) {
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;
    for (n = 0; n < ms->possible_cpus->len; n++) {
        ms->possible_cpus->cpus[n].type = ms->cpu_type;
        ms->possible_cpus->cpus[n].arch_id =
            arm_build_mp_affinity(n, GICV3_TARGETLIST_BITS);
        ms->possible_cpus->cpus[n].props.has_thread_id = true;
        ms->possible_cpus->cpus[n].props.thread_id = n;
    }
    return ms->possible_cpus;
}

static bool vmapple_get_vgic_bases(const MachineState *ms,
                                   uint64_t *dist_base, uint64_t *redist_base)
{
    *dist_base = memmap[VMAPPLE_GIC_DIST].base;
    *redist_base = memmap[VMAPPLE_GIC_REDIST].base;
    return true;
}

static GlobalProperty vmapple_compat_defaults[] = {
    { TYPE_VIRTIO_PCI, "disable-legacy", "on" },
    /*
     * macOS XHCI driver attempts to schedule events onto even rings 1 & 2
     * even when (as here) there is no MSI(-X) support. Disabling interrupter
     * mapping in the XHCI controller works around the problem.
     */
    { TYPE_XHCI_PCI, "conditional-intr-mapping", "on" },
};

static char *vmapple_get_tablet(Object *obj, Error **errp)
{
    VMAppleMachineState *vms = VMAPPLE_MACHINE(obj);

    return g_strdup(vms->tablet);
}

static void vmapple_set_tablet(Object *obj, const char *value, Error **errp)
{
    VMAppleMachineState *vms = VMAPPLE_MACHINE(obj);
    ObjectClass *oc = object_class_by_name(value);

    if (!oc || object_class_is_abstract(oc) ||
        !object_class_dynamic_cast(oc, TYPE_USB_DEVICE)) {
        error_setg(errp, "tablet: '%s' is not a USB device", value);
        return;
    }
    g_free(vms->tablet);
    vms->tablet = g_strdup(value);
}

static void vmapple_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = mach_vmapple_init;
    mc->max_cpus = 32;
    mc->block_default_type = IF_VIRTIO;
    mc->no_cdrom = 1;
    mc->pci_allow_0_address = true;
    mc->minimum_page_bits = 12;
    mc->possible_cpu_arch_ids = vmapple_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = vmapple_cpu_index_to_props;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("host");
    mc->get_default_cpu_node_id = vmapple_get_default_cpu_node_id;
    mc->get_vgic_bases = vmapple_get_vgic_bases;
    mc->default_ram_id = "mach-vmapple.ram";
    mc->desc = "Apple aarch64 Virtual Machine";

    compat_props_add(mc->compat_props, vmapple_compat_defaults,
                     G_N_ELEMENTS(vmapple_compat_defaults));

    object_class_property_add_str(oc, "tablet", vmapple_get_tablet,
                                  vmapple_set_tablet);
    object_class_property_set_description(oc, "tablet",
                                          "The USB pointing device: "
                                          "usb-vmapple-tablet (default), "
                                          "or another USB device such as "
                                          "usb-tablet");
}

static void vmapple_instance_init(Object *obj)
{
    VMAppleMachineState *vms = VMAPPLE_MACHINE(obj);

    vms->irqmap = irqmap;
    /*
     * macOS drives Virtualization.framework's USB tablet with its own
     * driver, which takes smooth scrolling.
     */
    vms->tablet = g_strdup("usb-vmapple-tablet");

    object_property_add_uint64_ptr(obj, "uuid", &vms->uuid,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "uuid", "Machine UUID (SDOM)");
}

static void vmapple_instance_finalize(Object *obj)
{
    VMAppleMachineState *vms = VMAPPLE_MACHINE(obj);

    g_free(vms->tablet);
}

static const TypeInfo vmapple_machine_info = {
    .name          = TYPE_VMAPPLE_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(VMAppleMachineState),
    .class_init    = vmapple_machine_class_init,
    .instance_init = vmapple_instance_init,
    .instance_finalize = vmapple_instance_finalize,
};

static void machvmapple_machine_init(void)
{
    type_register_static(&vmapple_machine_info);
}
type_init(machvmapple_machine_init);

