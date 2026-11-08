/*
 * VMApple RTC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Beside its PL031, Virtualization.framework's machine has a clock that the
 * guest's device tree lists as avp-rtc ("avp,rtc"), and that macOS drives
 * with its AppleVirtualPlatformRTC driver. The driver reads the device's
 * counter of microseconds and keeps the offset from it to calendar time in
 * NVRAM, as com.apple.System.rtc-offset, which it sets whenever the guest
 * sets its clock. It gives that offset to userspace as
 * kern.monotoniclock_offset_usecs, from which timed tells whether the clock
 * was reset. macOS's PL031 driver has no offset, so on the PL031 alone timed
 * takes the clock for reset on every boot, and sets it back to the last
 * write to the guest's filesystem.
 *
 * The counter counts microseconds of QEMU's rtc clock from the Unix epoch,
 * as the PL031 counts seconds, so -rtc base= and clock= apply to both. The
 * driver uses 64-bit accesses; the registers are:
 *
 *   0x00  COUNTER   read only, the counter
 *   0x18  CTRL      bit 0 enables the clock; reads back what was written
 *   0x20  INT_EN    interrupt enables; reads back what was written
 *   0x30  INT_STAT  pending interrupts; always 0
 *   0x38  INT_CLR   write ones to clear pending interrupts
 *   0x40  ID        reads 0xf001
 *
 * The driver passes interrupts 1 and 2 on to its user clients. Nothing here
 * raises them, so the device has no interrupt line. iBoot keeps avp-rtc in
 * the device tree and drops pl031-rtc when ID reads 0xf001, and the reverse
 * otherwise.
 */

#include "qemu/osdep.h"
#include "trace.h"
#include "hw/core/sysbus.h"
#include "hw/vmapple/vmapple.h"
#include "qemu/bitops.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/rtc.h"
#include "system/system.h"

OBJECT_DECLARE_SIMPLE_TYPE(VMAppleRTCState, VMAPPLE_RTC)

#define VMAPPLE_RTC_SIZE 0x1000

#define REG_COUNTER  0x00
#define REG_CTRL     0x18
#define REG_INT_EN   0x20
#define REG_INT_STAT 0x30
#define REG_INT_CLR  0x38
#define REG_ID       0x40

#define VMAPPLE_RTC_ID 0xf001

struct VMAppleRTCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    /* Seconds from the rtc clock to the guest's time, as the PL031 keeps */
    int64_t tick_offset;
    uint64_t ctrl;
    uint64_t int_en;
};

static uint64_t vmapple_rtc_usecs(VMAppleRTCState *s)
{
    int64_t now = qemu_clock_get_ns(rtc_clock);

    return (s->tick_offset * NANOSECONDS_PER_SECOND + now) / SCALE_US;
}

static uint64_t vmapple_rtc_reg(VMAppleRTCState *s, hwaddr reg)
{
    switch (reg) {
    case REG_COUNTER:
        return vmapple_rtc_usecs(s);
    case REG_CTRL:
        return s->ctrl;
    case REG_INT_EN:
        return s->int_en;
    case REG_INT_STAT:
    case REG_INT_CLR:
        return 0;
    case REG_ID:
        return VMAPPLE_RTC_ID;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%" HWADDR_PRIx "\n",
                      __func__, reg);
        return 0;
    }
}

static uint64_t vmapple_rtc_read(void *opaque, hwaddr offset, unsigned size)
{
    VMAppleRTCState *s = opaque;
    unsigned shift = (offset & 7) * BITS_PER_BYTE;
    uint64_t val;

    val = extract64(vmapple_rtc_reg(s, offset & ~7ULL), shift,
                    size * BITS_PER_BYTE);
    trace_vmapple_rtc_read(offset, size, val);
    return val;
}

static void vmapple_rtc_write(void *opaque, hwaddr offset, uint64_t val,
                              unsigned size)
{
    VMAppleRTCState *s = opaque;
    unsigned shift = (offset & 7) * BITS_PER_BYTE;
    unsigned len = size * BITS_PER_BYTE;

    trace_vmapple_rtc_write(offset, size, val);
    switch (offset & ~7ULL) {
    case REG_CTRL:
        s->ctrl = deposit64(s->ctrl, shift, len, val);
        break;
    case REG_INT_EN:
        s->int_en = deposit64(s->int_en, shift, len, val);
        break;
    case REG_INT_CLR:
        /* No interrupt is ever pending. */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only or unknown register 0x%"
                      HWADDR_PRIx "\n", __func__, offset);
        break;
    }
}

static const MemoryRegionOps vmapple_rtc_ops = {
    .read = vmapple_rtc_read,
    .write = vmapple_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    /* Whole, so that the counter's halves come from one reading. */
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void vmapple_rtc_reset(Object *obj, ResetType type)
{
    VMAppleRTCState *s = VMAPPLE_RTC(obj);

    s->ctrl = 0;
    s->int_en = 0;
}

static void vmapple_rtc_init(Object *obj)
{
    VMAppleRTCState *s = VMAPPLE_RTC(obj);
    struct tm tm;

    memory_region_init_io(&s->iomem, obj, &vmapple_rtc_ops, s, "vmapple-rtc",
                          VMAPPLE_RTC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);

    qemu_get_timedate(&tm, 0);
    s->tick_offset = mktimegm(&tm) -
        qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND;
}

static void vmapple_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "VMApple RTC";
    rc->phases.hold = vmapple_rtc_reset;
}

static const TypeInfo vmapple_rtc_info = {
    .name          = TYPE_VMAPPLE_RTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VMAppleRTCState),
    .instance_init = vmapple_rtc_init,
    .class_init    = vmapple_rtc_class_init,
};

static void vmapple_rtc_register_types(void)
{
    type_register_static(&vmapple_rtc_info);
}

type_init(vmapple_rtc_register_types)
