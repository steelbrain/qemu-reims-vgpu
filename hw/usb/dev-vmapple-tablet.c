/*
 * USB tablet of Virtualization.framework's macOS guests
 *
 * The USB pointing device Virtualization.framework gives a macOS guest: an
 * absolute pointer whose reports also carry smooth scrolling, that is, the
 * scroll distance in pixels with the phases of the gesture it belongs to
 * and of the momentum scrolling after it.  macOS binds its own driver,
 * AppleVirtualPlatformHIDEventDriver, to the device's vendor and product
 * IDs, and scrolls as it does with a trackpad.  Other guests see a HID
 * mouse with an absolute pointer, a wheel and a horizontal wheel.
 *
 * Report 1, in the input direction:
 *   byte  0     buttons 1 to 3
 *   bytes 1-4   X and Y, 0 to 0x7fff
 *   bytes 5-6   wheel notches, above 0 up
 *   bytes 7-8   horizontal wheel notches (AC Pan), above 0 right
 *   bytes 9-12  vertical scroll in 1/65536 line, above 0 up
 *   bytes 13-16 horizontal scroll in 1/65536 line, above 0 left
 *   bytes 17-18 phases: bits 1:0 momentum (none, began, changed, ended),
 *               bits 4:2 gesture (none, may begin, began, changed, ended,
 *               cancelled)
 * Feature report 2 is the wheels' resolution multiplier: set, the wheels
 * count 1/120 notch.  Feature report 3 bit 0 turns on the smooth scrolling
 * fields; this device fills them in either way.
 *
 * macOS takes a report as a scroll when one of its scroll fields is new: a
 * relative field that is not 0 or was not 0 in the report before, phases
 * that differ from the report before, or any field of the first report it
 * reads.  It then ignores the report's pointer and buttons, and it moves the
 * pointer or presses a button only for a field that differs from the report
 * before.  So a report of nothing goes first after a reset, scroll reports
 * repeat the pointer and buttons of the report before them, and a report
 * that returns the scroll fields to 0 comes between a scroll report and a
 * pointer report.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/input.h"
#include "hw/usb/usb.h"
#include "hw/usb/hid.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "desc.h"
#include "trace.h"
#include "qom/object.h"

#define VMAPPLE_TABLET_QUEUE_LENGTH 16

/* macOS scrolls 10 pixels a line of the smooth scrolling fields. */
#define VMAPPLE_TABLET_LINE_PIXELS 10

/*
 * The pixels a wheel notch stands for: what a notch of a wheel scrolls, and
 * the smooth scrolling that makes a notch in the wheel fields.  macOS does
 * not accelerate this device's scrolling; a notch of usb-tablet's wheel
 * scrolls from 5 to 40 pixels there, by how fast the wheel turns.
 */
#define VMAPPLE_TABLET_NOTCH_PIXELS 20

/* Wheel units a notch makes when the resolution multiplier is set */
#define VMAPPLE_TABLET_MULTIPLIED_NOTCH 120

enum {
    VMAPPLE_TABLET_REPORT_INPUT = 1,
    VMAPPLE_TABLET_REPORT_MULTIPLIER = 2,
    VMAPPLE_TABLET_REPORT_SMOOTH = 3,
};

#define VMAPPLE_TABLET_REPORT_SIZE 20

typedef struct VMAppleTabletReport {
    uint8_t buttons;
    uint16_t x;
    uint16_t y;
    int16_t wheel;
    int16_t pan;
    int32_t scroll_y;
    int32_t scroll_x;
    uint16_t phases;
} VMAppleTabletReport;

struct USBVMAppleTabletState {
    USBDevice dev;
    USBEndpoint *intr;
    QemuInputHandlerState *hs;
    bool input_active;
    char *display;
    uint32_t head;

    /* Input since the last synchronization */
    uint8_t buttons;
    uint16_t x;
    uint16_t y;
    bool pointer_changed;
    int32_t notches_y;
    int32_t notches_x;
    int64_t scroll_y;
    int64_t scroll_x;
    uint16_t phases;
    bool scrolled;

    /*
     * Smooth scrolling not yet a whole wheel unit, in 1/256 pixel times the
     * units a notch makes
     */
    int32_t notch_rest_y;
    int32_t notch_rest_x;

    VMAppleTabletReport queue[VMAPPLE_TABLET_QUEUE_LENGTH];
    uint32_t queue_head;
    uint32_t queue_count;
    /* The last report queued, which the guest reads last */
    VMAppleTabletReport last;
    /* Reports were dropped, the queue being full; see vmapple_tablet_push() */
    bool overflow;
    /* The phases of the last scroll report dropped that had some */
    uint16_t owed_phases;

    uint8_t idle;
    uint8_t multiplier;
    uint8_t smooth;
};

#define TYPE_USB_VMAPPLE_TABLET "usb-vmapple-tablet"
OBJECT_DECLARE_SIMPLE_TYPE(USBVMAppleTabletState, USB_VMAPPLE_TABLET)

enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT,
    STR_SERIALNUMBER,
    STR_CONFIG,
};

static const USBDescStrings desc_strings = {
    [STR_MANUFACTURER] = "QEMU",
    [STR_PRODUCT]      = "QEMU vmapple USB Tablet",
    [STR_SERIALNUMBER] = "1",
    [STR_CONFIG]       = "HID Tablet",
};

/* The descriptor Virtualization.framework's device has. */
static const uint8_t vmapple_tablet_report_descriptor[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x02,             /* Usage (Mouse) */
    0xa1, 0x01,             /* Collection (Application) */
    0x85, 0x01,             /*   Report ID (1) */
    0x09, 0x01,             /*   Usage (Pointer) */
    0xa1, 0x00,             /*   Collection (Physical) */
    0x05, 0x09,             /*     Usage Page (Button) */
    0x19, 0x01,             /*     Usage Minimum (1) */
    0x29, 0x03,             /*     Usage Maximum (3) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x25, 0x01,             /*     Logical Maximum (1) */
    0x95, 0x03,             /*     Report Count (3) */
    0x75, 0x01,             /*     Report Size (1) */
    0x81, 0x02,             /*     Input (Data, Variable, Absolute) */
    0x95, 0x01,             /*     Report Count (1) */
    0x75, 0x05,             /*     Report Size (5) */
    0x81, 0x01,             /*     Input (Constant) */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x30,             /*     Usage (X) */
    0x09, 0x31,             /*     Usage (Y) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x26, 0xff, 0x7f,       /*     Logical Maximum (0x7fff) */
    0x75, 0x10,             /*     Report Size (16) */
    0x95, 0x02,             /*     Report Count (2) */
    0x81, 0x02,             /*     Input (Data, Variable, Absolute) */
    0xa1, 0x02,             /*     Collection (Logical) */
    0x85, 0x02,             /*       Report ID (2) */
    0x09, 0x48,             /*       Usage (Resolution Multiplier) */
    0x95, 0x01,             /*       Report Count (1) */
    0x75, 0x08,             /*       Report Size (8) */
    0x15, 0x00,             /*       Logical Minimum (0) */
    0x25, 0x01,             /*       Logical Maximum (1) */
    0x35, 0x01,             /*       Physical Minimum (1) */
    0x45, 0x78,             /*       Physical Maximum (120) */
    0xb1, 0x02,             /*       Feature (Data, Variable, Absolute) */
    0x85, 0x01,             /*       Report ID (1) */
    0x09, 0x38,             /*       Usage (Wheel) */
    0x35, 0x00,             /*       Physical Minimum (0) */
    0x45, 0x00,             /*       Physical Maximum (0) */
    0x16, 0x01, 0x80,       /*       Logical Minimum (-0x7fff) */
    0x26, 0xff, 0x7f,       /*       Logical Maximum (0x7fff) */
    0x75, 0x10,             /*       Report Size (16) */
    0x95, 0x01,             /*       Report Count (1) */
    0x81, 0x06,             /*       Input (Data, Variable, Relative) */
    0x05, 0x0c,             /*       Usage Page (Consumer) */
    0x0a, 0x38, 0x02,       /*       Usage (AC Pan) */
    0x81, 0x06,             /*       Input (Data, Variable, Relative) */
    0x06, 0x61, 0xff,       /*       Usage Page (0xff61, vendor-defined) */
    0x09, 0x02,             /*       Usage (2): vertical scroll */
    0x09, 0x01,             /*       Usage (1): horizontal scroll */
    0x75, 0x20,             /*       Report Size (32) */
    0x95, 0x02,             /*       Report Count (2) */
    0x81, 0x06,             /*       Input (Data, Variable, Relative) */
    0x09, 0x03,             /*       Usage (3): phases */
    0x75, 0x10,             /*       Report Size (16) */
    0x95, 0x01,             /*       Report Count (1) */
    0x81, 0x02,             /*       Input (Data, Variable, Absolute) */
    0x85, 0x03,             /*       Report ID (3) */
    0x09, 0x04,             /*       Usage (4): smooth scrolling on */
    0x95, 0x01,             /*       Report Count (1) */
    0x75, 0x01,             /*       Report Size (1) */
    0xb1, 0x02,             /*       Feature (Data, Variable, Absolute) */
    0x95, 0x01,             /*       Report Count (1) */
    0x75, 0x07,             /*       Report Size (7) */
    0xb1, 0x01,             /*       Feature (Constant) */
    0xc0,                   /*     End Collection */
    0xc0,                   /*   End Collection */
    0xc0,                   /* End Collection */
};

static const uint8_t desc_hid_vmapple_tablet[] = {
    0x09,          /*  u8  bLength */
    USB_DT_HID,    /*  u8  bDescriptorType */
    0x11, 0x01,    /*  u16 HID_class */
    0x00,          /*  u8  country_code */
    0x01,          /*  u8  num_descriptors */
    USB_DT_REPORT, /*  u8  type: Report */
    sizeof(vmapple_tablet_report_descriptor), 0, /*  u16 len */
};

static const USBDescIface desc_iface_vmapple_tablet_full = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        { .data = desc_hid_vmapple_tablet },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = VMAPPLE_TABLET_REPORT_SIZE,
            .bInterval             = 1, /* 1 ms */
        },
    },
};

static const USBDescIface desc_iface_vmapple_tablet = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        { .data = desc_hid_vmapple_tablet },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = VMAPPLE_TABLET_REPORT_SIZE,
            .bInterval             = 4, /* 2 ^ (4 - 1) * 125 usecs = 1 ms */
        },
    },
};

static const USBDescDevice desc_device_vmapple_tablet_full = {
    .bcdUSB                        = 0x0110,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG,
            .bmAttributes          = USB_CFG_ATT_ONE,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_vmapple_tablet_full,
        },
    },
};

static const USBDescDevice desc_device_vmapple_tablet = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG,
            .bmAttributes          = USB_CFG_ATT_ONE,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_vmapple_tablet,
        },
    },
};

static const USBDesc desc_vmapple_tablet = {
    .id = {
        /* The IDs macOS binds AppleVirtualPlatformHIDEventDriver to */
        .idVendor          = 0x05ac,
        .idProduct         = 0x8106,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT,
        .iSerialNumber     = STR_SERIALNUMBER,
    },
    .full = &desc_device_vmapple_tablet_full,
    .high = &desc_device_vmapple_tablet,
    .str  = desc_strings,
};

static bool vmapple_tablet_report_scrolls(const VMAppleTabletReport *r)
{
    return r->wheel || r->pan || r->scroll_y || r->scroll_x || r->phases;
}

static void vmapple_tablet_enqueue(USBVMAppleTabletState *s,
                                   const VMAppleTabletReport *r)
{
    assert(s->queue_count < VMAPPLE_TABLET_QUEUE_LENGTH);
    s->queue[(s->queue_head + s->queue_count++) %
             VMAPPLE_TABLET_QUEUE_LENGTH] = *r;
    s->last = *r;
}

/*
 * Queue @r, after a report that returns the scroll fields to 0 if needed.
 *
 * A full queue drops @r, and every report after it until the guest has read
 * the queue empty, which then gets what the guest missed, from
 * vmapple_tablet_resync().  Merging @r into the last report queued instead
 * would lose it where the two differ in kind: macOS ignores the pointer and
 * buttons of a scroll report, and then sees no change in a pointer report
 * that repeats them, so a button released while the queue is full would
 * stay held.
 */
static void vmapple_tablet_push(USBVMAppleTabletState *s,
                                const VMAppleTabletReport *r)
{
    bool settle = vmapple_tablet_report_scrolls(&s->last) &&
                  !vmapple_tablet_report_scrolls(r);

    trace_usb_vmapple_tablet_report(r->buttons, r->x, r->y, r->wheel, r->pan,
                                    r->scroll_y, r->scroll_x, r->phases);
    if (s->overflow ||
        s->queue_count + settle >= VMAPPLE_TABLET_QUEUE_LENGTH) {
        trace_usb_vmapple_tablet_queue_full();
        s->overflow = true;
        if (r->phases) {
            s->owed_phases = r->phases;
        }
        return;
    }
    if (settle) {
        VMAppleTabletReport zero = {
            .buttons = s->last.buttons,
            .x = s->last.x,
            .y = s->last.y,
        };

        vmapple_tablet_enqueue(s, &zero);
    }
    vmapple_tablet_enqueue(s, r);
}

/*
 * Once the guest has read the queue empty after reports were dropped: the
 * phases of the last scrolling dropped, so that a gesture the guest missed
 * the end of ends, and the pointer and buttons as they are.
 */
static void vmapple_tablet_resync(USBVMAppleTabletState *s)
{
    VMAppleTabletReport scroll = {
        .buttons = s->last.buttons,
        .x = s->last.x,
        .y = s->last.y,
        .phases = s->owed_phases,
    };
    VMAppleTabletReport pointer = {
        .buttons = s->buttons,
        .x = s->x,
        .y = s->y,
    };

    s->overflow = false;
    s->owed_phases = 0;
    if (scroll.phases) {
        vmapple_tablet_push(s, &scroll);
    }
    vmapple_tablet_push(s, &pointer);
}

static uint16_t vmapple_tablet_phases(InputScrollPhase phase,
                                      InputScrollMomentum momentum)
{
    static const uint8_t gesture[INPUT_SCROLL_PHASE__MAX] = {
        [INPUT_SCROLL_PHASE_NONE] = 0,
        [INPUT_SCROLL_PHASE_MAY_BEGIN] = 1,
        [INPUT_SCROLL_PHASE_BEGAN] = 2,
        [INPUT_SCROLL_PHASE_CHANGED] = 3,
        [INPUT_SCROLL_PHASE_ENDED] = 4,
        [INPUT_SCROLL_PHASE_CANCELLED] = 5,
    };
    static const uint8_t momentum_phase[INPUT_SCROLL_MOMENTUM__MAX] = {
        [INPUT_SCROLL_MOMENTUM_NONE] = 0,
        [INPUT_SCROLL_MOMENTUM_BEGAN] = 1,
        [INPUT_SCROLL_MOMENTUM_CHANGED] = 2,
        [INPUT_SCROLL_MOMENTUM_ENDED] = 3,
    };

    return gesture[phase] << 2 | momentum_phase[momentum];
}

static int64_t vmapple_tablet_notch_units(USBVMAppleTabletState *s)
{
    return s->multiplier ? VMAPPLE_TABLET_MULTIPLIED_NOTCH : 1;
}

/*
 * @distance in 1/256 pixel as whole wheel units, keeping the rest in @rest,
 * which counts afresh when the direction turns.
 */
static int64_t vmapple_tablet_wheel(USBVMAppleTabletState *s, int32_t *rest,
                                    int64_t distance)
{
    const int64_t notch = VMAPPLE_TABLET_NOTCH_PIXELS * 256;
    int64_t sum = distance * vmapple_tablet_notch_units(s);
    int64_t units;

    if (!sum) {
        return 0;
    }
    if ((int64_t)*rest * sum > 0) {
        sum += *rest;
    }
    units = sum / notch;
    *rest = sum - units * notch;
    return units;
}

/* @distance in 1/256 pixel in 1/65536 line */
static int32_t vmapple_tablet_lines(int64_t distance)
{
    int64_t lines = distance * 256 / VMAPPLE_TABLET_LINE_PIXELS;

    return MIN(MAX(lines, INT32_MIN), INT32_MAX);
}

/*
 * @distance plus @value, in 1/256 pixel, held to 32 bits: input-send-event
 * takes any 64-bit value, which the sums after this would overflow.
 */
static int64_t vmapple_tablet_add(int64_t distance, int64_t value)
{
    value = MIN(MAX(value, INT32_MIN), INT32_MAX);
    return MIN(MAX(distance + value, INT32_MIN), INT32_MAX);
}

static void vmapple_tablet_input_event(DeviceState *dev, QemuConsole *src,
                                       QemuInputEvent *evt)
{
    static const uint8_t bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT]   = 0x01,
        [INPUT_BUTTON_RIGHT]  = 0x02,
        [INPUT_BUTTON_MIDDLE] = 0x04,
    };
    USBVMAppleTabletState *s = USB_VMAPPLE_TABLET(dev);

    switch (evt->type) {
    case INPUT_EVENT_KIND_BTN:
        switch (evt->btn.button) {
        case INPUT_BUTTON_WHEEL_UP:
            s->notches_y += evt->btn.down;
            break;
        case INPUT_BUTTON_WHEEL_DOWN:
            s->notches_y -= evt->btn.down;
            break;
        case INPUT_BUTTON_WHEEL_LEFT:
            s->notches_x += evt->btn.down;
            break;
        case INPUT_BUTTON_WHEEL_RIGHT:
            s->notches_x -= evt->btn.down;
            break;
        default:
            if (evt->btn.down) {
                s->buttons |= bmap[evt->btn.button];
            } else {
                s->buttons &= ~bmap[evt->btn.button];
            }
            s->pointer_changed = true;
            break;
        }
        break;

    case INPUT_EVENT_KIND_ABS:
        if (evt->abs.axis == INPUT_AXIS_X) {
            s->x = evt->abs.value;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            s->y = evt->abs.value;
        }
        s->pointer_changed = true;
        break;

    case INPUT_EVENT_KIND_SCROLL:
        if (evt->scroll.axis == INPUT_AXIS_X) {
            s->scroll_x = vmapple_tablet_add(s->scroll_x, evt->scroll.value);
        } else if (evt->scroll.axis == INPUT_AXIS_Y) {
            s->scroll_y = vmapple_tablet_add(s->scroll_y, evt->scroll.value);
        }
        s->phases = vmapple_tablet_phases(evt->scroll.phase,
                                          evt->scroll.momentum);
        s->scrolled = true;
        break;

    default:
        break;
    }
}

static void vmapple_tablet_input_sync(DeviceState *dev)
{
    USBVMAppleTabletState *s = USB_VMAPPLE_TABLET(dev);
    bool pushed = false;

    /* The pointer first, so that the scrolling is where the pointer went. */
    if (s->pointer_changed) {
        VMAppleTabletReport r = {
            .buttons = s->buttons,
            .x = s->x,
            .y = s->y,
        };

        vmapple_tablet_push(s, &r);
        pushed = true;
    }
    if (s->scrolled || s->notches_y || s->notches_x) {
        const int64_t notch = VMAPPLE_TABLET_NOTCH_PIXELS * 256;
        int64_t units = vmapple_tablet_notch_units(s);
        int64_t wheel = s->notches_y * units +
            vmapple_tablet_wheel(s, &s->notch_rest_y, s->scroll_y);
        int64_t left = s->notches_x * units +
            vmapple_tablet_wheel(s, &s->notch_rest_x, s->scroll_x);
        VMAppleTabletReport r = {
            .buttons = s->last.buttons,
            .x = s->last.x,
            .y = s->last.y,
            .wheel = MIN(MAX(wheel, -INT16_MAX), INT16_MAX),
            /* AC Pan counts to the right, as HID has it */
            .pan = MIN(MAX(-left, -INT16_MAX), INT16_MAX),
            .scroll_y = vmapple_tablet_lines(s->scroll_y +
                                             s->notches_y * notch),
            .scroll_x = vmapple_tablet_lines(s->scroll_x +
                                             s->notches_x * notch),
            .phases = s->scrolled ? s->phases : 0,
        };

        vmapple_tablet_push(s, &r);
        pushed = true;
    }

    s->pointer_changed = false;
    s->notches_y = 0;
    s->notches_x = 0;
    s->scroll_y = 0;
    s->scroll_x = 0;
    s->scrolled = false;
    if (pushed) {
        usb_wakeup(s->intr, 0);
    }
}

static const QemuInputHandler vmapple_tablet_handler = {
    .name  = "QEMU vmapple USB Tablet",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS |
             INPUT_EVENT_MASK_SCROLL,
    .event = vmapple_tablet_input_event,
    .sync  = vmapple_tablet_input_sync,
};

static int vmapple_tablet_write_report(const VMAppleTabletReport *r,
                                       uint8_t *buf, int len)
{
    uint8_t report[VMAPPLE_TABLET_REPORT_SIZE];

    report[0] = VMAPPLE_TABLET_REPORT_INPUT;
    report[1] = r->buttons;
    stw_le_p(&report[2], r->x);
    stw_le_p(&report[4], r->y);
    stw_le_p(&report[6], r->wheel);
    stw_le_p(&report[8], r->pan);
    stl_le_p(&report[10], r->scroll_y);
    stl_le_p(&report[14], r->scroll_x);
    stw_le_p(&report[18], r->phases);

    len = MIN(len, sizeof(report));
    memcpy(buf, report, len);
    return len;
}

static void usb_vmapple_tablet_handle_reset(USBDevice *dev)
{
    USBVMAppleTabletState *s = USB_VMAPPLE_TABLET(dev);
    const VMAppleTabletReport nothing = { 0 };

    s->buttons = 0;
    s->pointer_changed = false;
    s->notches_y = 0;
    s->notches_x = 0;
    s->scroll_y = 0;
    s->scroll_x = 0;
    s->phases = 0;
    s->scrolled = false;
    s->notch_rest_y = 0;
    s->notch_rest_x = 0;
    s->queue_head = 0;
    s->queue_count = 0;
    s->overflow = false;
    s->owed_phases = 0;
    /* The first report the guest reads, which it takes as a scroll */
    vmapple_tablet_enqueue(s, &nothing);
    s->idle = 0;
    s->multiplier = 0;
    s->smooth = 0;
}

static void usb_vmapple_tablet_handle_control(USBDevice *dev, USBPacket *p,
                                              int request, int value,
                                              int index, int length,
                                              uint8_t *data)
{
    USBVMAppleTabletState *s = USB_VMAPPLE_TABLET(dev);
    uint8_t type = value >> 8;
    uint8_t id = value & 0xff;
    int ret;

    ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }

    switch (request) {
    case InterfaceRequest | USB_REQ_GET_DESCRIPTOR:
        if (type != USB_DT_REPORT) {
            goto fail;
        }
        length = MIN(length, sizeof(vmapple_tablet_report_descriptor));
        memcpy(data, vmapple_tablet_report_descriptor, length);
        p->actual_length = length;
        break;
    case HID_GET_REPORT:
        if (type == 1 && id == VMAPPLE_TABLET_REPORT_INPUT) {
            VMAppleTabletReport r = {
                .buttons = s->buttons,
                .x = s->x,
                .y = s->y,
            };

            p->actual_length = vmapple_tablet_write_report(&r, data, length);
        } else if (type == 3 && id == VMAPPLE_TABLET_REPORT_MULTIPLIER &&
                   length >= 2) {
            data[0] = id;
            data[1] = s->multiplier;
            p->actual_length = 2;
        } else if (type == 3 && id == VMAPPLE_TABLET_REPORT_SMOOTH &&
                   length >= 2) {
            data[0] = id;
            data[1] = s->smooth;
            p->actual_length = 2;
        } else {
            goto fail;
        }
        trace_usb_vmapple_tablet_get_report(type, id);
        break;
    case HID_SET_REPORT:
        if (type != 3 || length < 2 || data[0] != id) {
            goto fail;
        }
        if (id == VMAPPLE_TABLET_REPORT_MULTIPLIER) {
            s->multiplier = data[1] & 1;
            s->notch_rest_y = 0;
            s->notch_rest_x = 0;
        } else if (id == VMAPPLE_TABLET_REPORT_SMOOTH) {
            s->smooth = data[1] & 1;
        } else {
            goto fail;
        }
        trace_usb_vmapple_tablet_set_report(id, data[1]);
        break;
    case HID_GET_IDLE:
        data[0] = s->idle;
        p->actual_length = 1;
        break;
    case HID_SET_IDLE:
        s->idle = value >> 8;
        break;
    default:
    fail:
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_vmapple_tablet_handle_data(USBDevice *dev, USBPacket *p)
{
    USBVMAppleTabletState *s = USB_VMAPPLE_TABLET(dev);
    uint8_t buf[VMAPPLE_TABLET_REPORT_SIZE];
    int len;

    if (p->pid != USB_TOKEN_IN || p->ep->nr != 1) {
        p->status = USB_RET_STALL;
        return;
    }

    /* As usb-tablet does: take the input once the guest reads the device. */
    if (!s->input_active) {
        qemu_input_handler_activate(s->hs);
        s->input_active = true;
    }
    if (!s->queue_count && s->overflow) {
        vmapple_tablet_resync(s);
    }
    if (!s->queue_count) {
        p->status = USB_RET_NAK;
        return;
    }
    len = vmapple_tablet_write_report(&s->queue[s->queue_head], buf,
                                      p->iov.size);
    s->queue_head = (s->queue_head + 1) % VMAPPLE_TABLET_QUEUE_LENGTH;
    s->queue_count--;
    usb_packet_copy(p, buf, len);
}

static void usb_vmapple_tablet_unrealize(USBDevice *dev)
{
    USBVMAppleTabletState *s = USB_VMAPPLE_TABLET(dev);

    qemu_input_handler_unregister(s->hs);
}

static void usb_vmapple_tablet_realize(USBDevice *dev, Error **errp)
{
    ERRP_GUARD();
    USBVMAppleTabletState *s = USB_VMAPPLE_TABLET(dev);

    dev->usb_desc = &desc_vmapple_tablet;
    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    s->intr = usb_ep_get(dev, USB_TOKEN_IN, 1);
    s->hs = qemu_input_handler_register(DEVICE(dev), &vmapple_tablet_handler);
    if (s->display) {
        qemu_input_handler_bind(s->hs, s->display, s->head, errp);
        if (*errp) {
            /* A device that fails to realize is freed, not unrealized. */
            g_clear_pointer(&s->hs, qemu_input_handler_unregister);
        }
    }
}

static const VMStateDescription vmstate_vmapple_tablet_report = {
    .name = "usb-vmapple-tablet/report",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(buttons, VMAppleTabletReport),
        VMSTATE_UINT16(x, VMAppleTabletReport),
        VMSTATE_UINT16(y, VMAppleTabletReport),
        VMSTATE_INT16(wheel, VMAppleTabletReport),
        VMSTATE_INT16(pan, VMAppleTabletReport),
        VMSTATE_INT32(scroll_y, VMAppleTabletReport),
        VMSTATE_INT32(scroll_x, VMAppleTabletReport),
        VMSTATE_UINT16(phases, VMAppleTabletReport),
        VMSTATE_END_OF_LIST()
    }
};

static int usb_vmapple_tablet_post_load(void *opaque, int version_id)
{
    USBVMAppleTabletState *s = opaque;

    if (s->queue_head >= VMAPPLE_TABLET_QUEUE_LENGTH ||
        s->queue_count > VMAPPLE_TABLET_QUEUE_LENGTH) {
        return -EINVAL;
    }
    return 0;
}

static bool usb_vmapple_tablet_overflow_needed(void *opaque)
{
    USBVMAppleTabletState *s = opaque;

    return s->overflow;
}

static const VMStateDescription vmstate_usb_vmapple_tablet_overflow = {
    .name = "usb-vmapple-tablet/overflow",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = usb_vmapple_tablet_overflow_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(overflow, USBVMAppleTabletState),
        VMSTATE_UINT16(owed_phases, USBVMAppleTabletState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_usb_vmapple_tablet = {
    .name = "usb-vmapple-tablet",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = usb_vmapple_tablet_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_USB_DEVICE(dev, USBVMAppleTabletState),
        VMSTATE_UINT8(buttons, USBVMAppleTabletState),
        VMSTATE_UINT16(x, USBVMAppleTabletState),
        VMSTATE_UINT16(y, USBVMAppleTabletState),
        VMSTATE_INT32(notch_rest_y, USBVMAppleTabletState),
        VMSTATE_INT32(notch_rest_x, USBVMAppleTabletState),
        VMSTATE_STRUCT_ARRAY(queue, USBVMAppleTabletState,
                             VMAPPLE_TABLET_QUEUE_LENGTH, 1,
                             vmstate_vmapple_tablet_report,
                             VMAppleTabletReport),
        VMSTATE_UINT32(queue_head, USBVMAppleTabletState),
        VMSTATE_UINT32(queue_count, USBVMAppleTabletState),
        VMSTATE_STRUCT(last, USBVMAppleTabletState, 1,
                       vmstate_vmapple_tablet_report, VMAppleTabletReport),
        VMSTATE_UINT8(idle, USBVMAppleTabletState),
        VMSTATE_UINT8(multiplier, USBVMAppleTabletState),
        VMSTATE_UINT8(smooth, USBVMAppleTabletState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_usb_vmapple_tablet_overflow,
        NULL
    }
};

static const Property usb_vmapple_tablet_properties[] = {
    DEFINE_PROP_STRING("display", USBVMAppleTabletState, display),
    DEFINE_PROP_UINT32("head", USBVMAppleTabletState, head, 0),
};

static void usb_vmapple_tablet_class_init(ObjectClass *klass,
                                          const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->product_desc   = "QEMU vmapple USB Tablet";
    uc->realize        = usb_vmapple_tablet_realize;
    uc->unrealize      = usb_vmapple_tablet_unrealize;
    uc->handle_reset   = usb_vmapple_tablet_handle_reset;
    uc->handle_control = usb_vmapple_tablet_handle_control;
    uc->handle_data    = usb_vmapple_tablet_handle_data;
    uc->handle_attach  = usb_desc_attach;
    dc->desc = "USB tablet of Virtualization.framework's macOS guests";
    dc->vmsd = &vmstate_usb_vmapple_tablet;
    device_class_set_props(dc, usb_vmapple_tablet_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usb_vmapple_tablet_info = {
    .name          = TYPE_USB_VMAPPLE_TABLET,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBVMAppleTabletState),
    .class_init    = usb_vmapple_tablet_class_init,
};

static void usb_vmapple_tablet_register_types(void)
{
    type_register_static(&usb_vmapple_tablet_info);
}

type_init(usb_vmapple_tablet_register_types)
