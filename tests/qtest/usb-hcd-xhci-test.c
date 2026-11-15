/*
 * QTest testcase for USB xHCI controller
 *
 * Copyright (c) 2014 HUAWEI TECHNOLOGIES CO., LTD.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "libqos/usb.h"
#include "libqos/malloc-pc.h"
#include "qobject/qdict.h"

/* capability registers */
#define XHCI_CAPLENGTH          0x00
#define XHCI_HCSPARAMS1         0x04
#define XHCI_HCCPARAMS1         0x10
#define XHCI_DBOFF              0x14
#define XHCI_RTSOFF             0x18
/* operational registers */
#define XHCI_USBCMD             0x00
#define XHCI_USBSTS             0x04
#define XHCI_CRCR               0x18
#define XHCI_DCBAAP             0x30
#define XHCI_CONFIG             0x38
#define XHCI_PORTSC(n)          (0x400 + 0x10 * (n))
/* relative to the runtime registers */
#define XHCI_MFINDEX            0x00
/* interrupter 0, relative to the runtime registers */
#define XHCI_IMAN               0x20
#define XHCI_IMOD               0x24
#define XHCI_ERSTSZ             0x28
#define XHCI_ERSTBA             0x30
#define XHCI_ERDP               0x38

#define USBCMD_RS               (1 << 0)
#define USBCMD_HCRST            (1 << 1)
#define USBSTS_HCH              (1 << 0)
#define USBSTS_HCE              (1 << 12)
#define PORTSC_CCS              (1 << 0)
#define PORTSC_PR               (1 << 4)
#define PORTSC_PP               (1 << 9)
#define CRCR_RCS                (1 << 0)
#define ERDP_EHB                (1 << 3)
#define IMAN_IP                 (1 << 0)
#define HCCP1_LTC               (1 << 6)

#define TRB_C                   (1 << 0)
#define TRB_TR_ISP              (1 << 2)
#define TRB_TR_CH               (1 << 4)
#define TRB_TR_IOC              (1 << 5)
#define TRB_TR_BEI              (1 << 9)
#define TRB_TR_SIA              (1U << 31)
#define TRB_TR_FRAMEID(f)       ((f) << 20)
#define TRB_TYPE(t)             ((t) << 10)
#define TRB_GET_TYPE(control)   (((control) >> 10) & 0x3f)
#define TRB_GET_CCODE(status)   ((status) >> 24)
#define TRB_GET_SLOT(control)   ((control) >> 24)
#define TRB_GET_VF(control)     (((control) >> 16) & 0xff)
#define TRB_GET_PARAM(status)   ((status) & 0xffffff)
#define TRB_CR_SLOT(s)          ((uint32_t)(s) << 24)
#define TRB_CR_EPID(e)          ((e) << 16)
#define TRB_CR_DEV_SPEED(s)     ((s) << 16)

#define TR_NORMAL              1
#define TR_ISOCH                5
#define CR_ENABLE_SLOT          9
#define CR_ADDRESS_DEVICE       11
#define CR_CONFIGURE_ENDPOINT   12
#define CR_STOP_ENDPOINT        15
#define CR_SET_TR_DEQUEUE       16
#define CR_SET_LATENCY_TOLERANCE 20
#define CR_GET_PORT_BANDWIDTH   21
#define CR_FORCE_HEADER         22
#define CR_NOOP                 23
#define ER_TRANSFER             32
#define ER_COMMAND_COMPLETE     33
#define ER_NEC_REPLY            48
#define CR_NEC_FIRMWARE_REVISION 49
#define CR_NEC_CHALLENGE_RESPONSE 50
#define CC_SUCCESS              1
#define CC_TRB_ERROR            5
#define CC_STALL_ERROR          6
#define CC_SLOT_NOT_ENABLED     11
#define CC_SHORT_PACKET         13
#define CC_RING_UNDERRUN        14
#define CC_RING_OVERRUN         15

#define SPEED_ID_HIGH           3

#define EP_TYPE_ISOCH_OUT       1
#define EP_TYPE_CONTROL         4
#define EP_TYPE_ISOCH_IN        5
#define EP_TYPE_INTR_IN         7

#define EP_STATE_MASK           0x7
#define EP_STATE_STOPPED        3

#define XHCI_RING_TRBS          64
#define XHCI_MICROFRAME_NS      125000
/* in the PCI hole of the pc machine, nothing is mapped there */
#define XHCI_UNASSIGNED_ADDR    0xf0000000

typedef struct XHCITest {
    QTestState *qts;
    QGuestAllocator alloc;
    QPCIBus *bus;
    struct qhc hc;
    uint32_t oper, runtime, doorbell;
    uint64_t dcbaa, cmd_ring, event_ring, input_ctx, dev_ctx;
    unsigned int cmd_idx, event_idx;
    unsigned int port, slot;
} XHCITest;

static void wait_device_deleted_event(QTestState *qtest, const char *id)
{
    QDict *resp, *data;
    const char *device;

    /*
     * Other devices might get removed along with the removed device. Skip
     * these. The device of interest will be the last one.
     */
    for (;;) {
        resp = qtest_qmp_eventwait_ref(qtest, "DEVICE_DELETED");
        data = qdict_get_qdict(resp, "data");
        device = data ? qdict_get_try_str(data, "device") : NULL;
        if (device && !strcmp(device, id)) {
            qobject_unref(resp);
            break;
        }
        qobject_unref(resp);
    }
}

/*
 * Regression test for the xHCI-PCI "host" strong-link reference cycle.
 *
 * The xHCI PCI wrapper embeds an xhci-core child whose strong "host" link
 * points back at the PCI device, forming a refcount cycle. If
 * usb_xhci_pci_exit() does not break that cycle, the device's refcount never
 * reaches 0 on unplug, device_finalize() never runs, and therefore the
 * DEVICE_DELETED event (emitted from device_finalize()) is never sent.
 *
 * This test hot-plugs an xHCI controller into an ACPI-hotpluggable bus,
 * requests its removal and waits for DEVICE_DELETED. Without the fix the event
 * is never delivered (device_finalize() is blocked), so the test would
 * hang/time out.
 */
static void test_xhci_unplug_finalize(void)
{
    QTestState *qtest;
    const char *arch = qtest_get_arch();

    if (strcmp(arch, "i386") != 0 && strcmp(arch, "x86_64") != 0) {
        g_test_skip("Test only runs on x86 (ACPI PCI hotplug)");
        return;
    }
    if (!qtest_has_device("nec-usb-xhci")) {
        g_test_skip("Device nec-usb-xhci not available");
        return;
    }

    qtest = qtest_initf("-machine pc");

    qtest_qmp_device_add(qtest, "nec-usb-xhci", "xhci-finalize", "{}");

    /*
     * Request device removal. As the guest is not running, the unplug request
     * won't be processed until the next system reset, which performs the
     * removal and triggers device_finalize() (and thus DEVICE_DELETED).
     */
    qtest_qmp_device_del_send(qtest, "xhci-finalize");
    qtest_system_reset_nowait(qtest);
    wait_device_deleted_event(qtest, "xhci-finalize");

    qtest_quit(qtest);
}

static void test_xhci_hotplug(void)
{
    usb_test_hotplug(global_qtest, "xhci", "1", NULL);
}

static void test_usb_uas_hotplug(void)
{
    QTestState *qts = global_qtest;

    qtest_qmp_device_add(qts, "usb-uas", "uas", "{}");
    qtest_qmp_device_add(qts, "scsi-hd", "scsihd", "{'drive': 'drive0'}");

    /* TODO:
        UAS HBA driver in libqos, to check that
        added disk is visible after BUS rescan
    */

    qtest_qmp_device_del(qts, "scsihd");
    qtest_qmp_device_del(qts, "uas");
}

static void test_usb_ccid_hotplug(void)
{
    QTestState *qts = global_qtest;

    qtest_qmp_device_add(qts, "usb-ccid", "ccid", "{}");
    qtest_qmp_device_del(qts, "ccid");
    /* check the device can be added again */
    qtest_qmp_device_add(qts, "usb-ccid", "ccid", "{}");
    qtest_qmp_device_del(qts, "ccid");
}

static uint32_t xhci_readl(XHCITest *x, uint32_t off)
{
    return qpci_io_readl(x->hc.dev, x->hc.bar, off);
}

static void xhci_writel(XHCITest *x, uint32_t off, uint32_t val)
{
    qpci_io_writel(x->hc.dev, x->hc.bar, off, val);
}

static void xhci_writeq(XHCITest *x, uint32_t off, uint64_t val)
{
    xhci_writel(x, off, val);
    xhci_writel(x, off + 4, val >> 32);
}

static uint64_t xhci_alloc_page(XHCITest *x)
{
    uint64_t addr = guest_alloc(&x->alloc, 0x1000);

    qtest_memset(x->qts, addr, 0, 0x1000);
    return addr;
}

static void xhci_write_trb(XHCITest *x, uint64_t addr, uint64_t parameter,
                           uint32_t status, uint32_t control)
{
    qtest_writeq(x->qts, addr, parameter);
    qtest_writel(x->qts, addr + 8, status);
    qtest_writel(x->qts, addr + 12, control);
}

/* Fetch the next event if there is one. Does not advance the clock. */
static bool xhci_next_event(XHCITest *x, uint32_t *status, uint32_t *control)
{
    uint64_t addr = x->event_ring + 16 * x->event_idx;
    uint32_t c = qtest_readl(x->qts, addr + 12);

    if (!(c & TRB_C)) {
        return false;
    }
    if (status) {
        *status = qtest_readl(x->qts, addr + 8);
    }
    if (control) {
        *control = c;
    }
    x->event_idx++;
    g_assert_cmpuint(x->event_idx, <, XHCI_RING_TRBS);
    xhci_writeq(x, x->runtime + XHCI_ERDP, (addr + 16) | ERDP_EHB);
    return true;
}

/* Submit a command and return its completion code. */
static unsigned int xhci_submit_command(XHCITest *x, uint64_t parameter,
                                        uint32_t control, unsigned int *slot)
{
    uint32_t status;

    g_assert_cmpuint(x->cmd_idx, <, XHCI_RING_TRBS);
    xhci_write_trb(x, x->cmd_ring + 16 * x->cmd_idx++, parameter, 0,
                   control | TRB_C);
    xhci_writel(x, x->doorbell, 0);

    g_assert_true(xhci_next_event(x, &status, &control));
    g_assert_cmpuint(TRB_GET_TYPE(control), ==, ER_COMMAND_COMPLETE);
    if (slot) {
        *slot = TRB_GET_SLOT(control);
    }
    return TRB_GET_CCODE(status);
}

/* Submit a command that has to succeed and return the slot ID. */
static unsigned int xhci_command(XHCITest *x, uint64_t parameter,
                                 uint32_t control)
{
    unsigned int slot;

    g_assert_cmpuint(xhci_submit_command(x, parameter, control, &slot), ==,
                     CC_SUCCESS);
    return slot;
}

/*
 * Start xHC hc with one USB device, run the controller, enable a slot for
 * the device and fill in the input context for Address Device.
 */
static void xhci_test_init_hc(XHCITest *x, const char *machine,
                              const char *hc, const char *usb_device)
{
    uint64_t erst, ep0_ring;
    unsigned int maxports;

    memset(x, 0, sizeof(*x));
    /* pit=off: a long clock step would run the i8254 timer all the way */
    x->qts = qtest_initf("-machine %s,pit=off -nodefaults "
                         "-device %s,id=xhci,addr=04.0 %s",
                         machine, hc, usb_device);
    pc_alloc_init(&x->alloc, x->qts, ALLOC_NO_FLAGS);
    x->bus = qpci_new_pc(x->qts, NULL);
    qusb_pci_init_one(x->bus, &x->hc, QPCI_DEVFN(4, 0), 0);

    x->oper = qpci_io_readb(x->hc.dev, x->hc.bar, XHCI_CAPLENGTH);
    x->runtime = xhci_readl(x, XHCI_RTSOFF) & ~0x1f;
    x->doorbell = xhci_readl(x, XHCI_DBOFF) & ~0x3;
    maxports = xhci_readl(x, XHCI_HCSPARAMS1) >> 24;

    xhci_writel(x, x->oper + XHCI_USBCMD, USBCMD_HCRST);
    g_assert_false(xhci_readl(x, x->oper + XHCI_USBCMD) & USBCMD_HCRST);

    x->dcbaa = xhci_alloc_page(x);
    erst = xhci_alloc_page(x);
    x->cmd_ring = xhci_alloc_page(x);
    x->event_ring = xhci_alloc_page(x);
    x->input_ctx = xhci_alloc_page(x);

    xhci_writel(x, x->oper + XHCI_CONFIG, 1);
    xhci_writeq(x, x->oper + XHCI_DCBAAP, x->dcbaa);
    qtest_writeq(x->qts, erst, x->event_ring);
    qtest_writel(x->qts, erst + 8, XHCI_RING_TRBS);
    xhci_writel(x, x->runtime + XHCI_ERSTSZ, 1);
    xhci_writeq(x, x->runtime + XHCI_ERSTBA, erst);
    xhci_writeq(x, x->runtime + XHCI_ERDP, x->event_ring | ERDP_EHB);
    xhci_writeq(x, x->oper + XHCI_CRCR, x->cmd_ring | CRCR_RCS);
    xhci_writel(x, x->oper + XHCI_USBCMD, USBCMD_RS);
    g_assert_false(xhci_readl(x, x->oper + XHCI_USBSTS) & USBSTS_HCH);

    for (x->port = 0; x->port < maxports; x->port++) {
        if (xhci_readl(x, x->oper + XHCI_PORTSC(x->port)) & PORTSC_CCS) {
            break;
        }
    }
    g_assert_cmpuint(x->port, <, maxports);
    xhci_writel(x, x->oper + XHCI_PORTSC(x->port), PORTSC_PP | PORTSC_PR);
    while (xhci_next_event(x, NULL, NULL)) {
        /* drop the port status change events */
    }

    x->slot = xhci_command(x, 0, TRB_TYPE(CR_ENABLE_SLOT));
    x->dev_ctx = xhci_alloc_page(x);
    qtest_writeq(x->qts, x->dcbaa + 8 * x->slot, x->dev_ctx);

    /* input control context: add slot and ep0 */
    qtest_writel(x->qts, x->input_ctx + 0x04, 0x3);
    /* slot context: one context entry, root hub port */
    qtest_writel(x->qts, x->input_ctx + 0x20, 1 << 27);
    qtest_writel(x->qts, x->input_ctx + 0x24, (x->port + 1) << 16);
    /* ep0 context */
    ep0_ring = xhci_alloc_page(x);
    qtest_writel(x->qts, x->input_ctx + 0x44,
                 (64 << 16) | (EP_TYPE_CONTROL << 3));
    qtest_writeq(x->qts, x->input_ctx + 0x48, ep0_ring | 1);
}

static void xhci_test_init(XHCITest *x, const char *usb_device)
{
    xhci_test_init_hc(x, "pc", "qemu-xhci", usb_device);
}

/* xhci_test_init_hc(), then bring the device to the Addressed state. */
static void xhci_test_start_hc(XHCITest *x, const char *machine,
                               const char *hc, const char *usb_device)
{
    xhci_test_init_hc(x, machine, hc, usb_device);
    xhci_command(x, x->input_ctx,
                 TRB_TYPE(CR_ADDRESS_DEVICE) | (x->slot << 24));
}

static void xhci_test_start_on(XHCITest *x, const char *machine,
                               const char *usb_device)
{
    xhci_test_start_hc(x, machine, "qemu-xhci", usb_device);
}

static void xhci_test_start(XHCITest *x, const char *usb_device)
{
    xhci_test_start_on(x, "pc", usb_device);
}

/* Returns the address of the transfer ring. */
static uint64_t xhci_configure_ep(XHCITest *x, unsigned int epid,
                                  unsigned int type, unsigned int interval,
                                  unsigned int max_packet)
{
    uint64_t ring = xhci_alloc_page(x);
    uint64_t epctx = x->input_ctx + 0x20 * (epid + 1);

    qtest_memset(x->qts, x->input_ctx, 0, 0x1000);
    qtest_writel(x->qts, x->input_ctx + 0x04, (1 << epid) | 1);
    qtest_writel(x->qts, x->input_ctx + 0x20, epid << 27);
    qtest_writel(x->qts, x->input_ctx + 0x24, (x->port + 1) << 16);
    qtest_writel(x->qts, epctx + 0x00, interval << 16);
    qtest_writel(x->qts, epctx + 0x04, (max_packet << 16) | (type << 3));
    qtest_writeq(x->qts, epctx + 0x08, ring | 1);
    xhci_command(x, x->input_ctx,
                 TRB_TYPE(CR_CONFIGURE_ENDPOINT) | (x->slot << 24));
    return ring;
}

static void xhci_test_end(XHCITest *x)
{
    g_free(x->hc.dev);
    qpci_free_pc(x->bus);
    alloc_destroy(&x->alloc);
    qtest_quit(x->qts);
}

static bool xhci_test_supported(const char *usb_device)
{
    const char *arch = qtest_get_arch();

    if (strcmp(arch, "i386") != 0 && strcmp(arch, "x86_64") != 0) {
        g_test_skip("Test only runs on x86 (pc machine)");
        return false;
    }
    if (!qtest_has_device("qemu-xhci") || !qtest_has_device(usb_device)) {
        g_test_skip("Devices not available");
        return false;
    }
    return true;
}

/*
 * Address Device reads the output context pointer from the DCBAA. Point
 * DCBAAP at unassigned memory: the command has to fail instead of carrying
 * on with the zero the failed read leaves behind, which used to put the
 * contexts at guest address 0 and still report Success.
 */
static void test_xhci_address_device_dcbaa_read_error(void)
{
    uint8_t fill[64], low_mem[sizeof(fill)];
    unsigned int ccode;
    XHCITest x;

    if (!xhci_test_supported("usb-kbd")) {
        return;
    }

    xhci_test_init(&x, "-device usb-kbd");
    memset(fill, 0xaa, sizeof(fill));
    qtest_memwrite(x.qts, 0, fill, sizeof(fill));
    xhci_writeq(&x, x.oper + XHCI_DCBAAP, XHCI_UNASSIGNED_ADDR);

    ccode = xhci_submit_command(&x, x.input_ctx,
                                TRB_TYPE(CR_ADDRESS_DEVICE) | (x.slot << 24),
                                NULL);
    g_assert_cmpuint(ccode, ==, CC_TRB_ERROR);
    g_assert_cmphex(xhci_readl(&x, x.oper + XHCI_USBSTS) & USBSTS_HCE, ==,
                    USBSTS_HCE);
    qtest_memread(x.qts, 0, low_mem, sizeof(low_mem));
    g_assert_cmpmem(low_mem, sizeof(low_mem), fill, sizeof(fill));

    /* The failed command left the slot usable. */
    xhci_writeq(&x, x.oper + XHCI_DCBAAP, x.dcbaa);
    xhci_command(&x, x.input_ctx,
                 TRB_TYPE(CR_ADDRESS_DEVICE) | (x.slot << 24));

    xhci_test_end(&x);
}

/*
 * An isoch TD with SIA set is run at the next interval boundary. That has to
 * hold once the microframe index no longer fits in 32 bits as well.
 */
static void test_xhci_isoch_mfindex_32bit(void)
{
    const unsigned int interval = 6;
    uint32_t control;
    uint64_t ring;
    XHCITest x;

    if (!xhci_test_supported("usb-audio")) {
        return;
    }

    xhci_test_start(&x, "-audiodev none,id=snd0 "
                        "-device usb-audio,audiodev=snd0");
    ring = xhci_configure_ep(&x, 2, EP_TYPE_ISOCH_OUT, interval, 64);

    /* Go past 2^32 microframes and stop off an interval boundary. */
    qtest_clock_step(x.qts, (1ULL << 32) * XHCI_MICROFRAME_NS);
    qtest_clock_step(x.qts, 5 * XHCI_MICROFRAME_NS);

    xhci_write_trb(&x, ring, xhci_alloc_page(&x), 64,
                   TRB_TYPE(TR_ISOCH) | TRB_TR_SIA | TRB_TR_IOC | TRB_C);
    xhci_writel(&x, x.doorbell + 4 * x.slot, 2);
    g_assert_false(xhci_next_event(&x, NULL, NULL));

    /*
     * The streaming interface has not been enabled, so usb-audio stalls the
     * TD. What matters is when that happens.
     */
    qtest_clock_step(x.qts, XHCI_MICROFRAME_NS << interval);
    g_assert_true(xhci_next_event(&x, NULL, &control));
    g_assert_cmpuint(TRB_GET_TYPE(control), ==, ER_TRANSFER);

    xhci_test_end(&x);
}

static void test_xhci_intr_mfindex_32bit(void)
{
    const unsigned int interval = 6;
    const uint8_t expected[] = { 0, 0, 4, 0, 0, 0, 0, 0 };
    uint8_t report[sizeof(expected)];
    uint32_t status, control;
    uint64_t ring, data;
    XHCITest x;

    if (!xhci_test_supported("usb-kbd")) {
        return;
    }

    xhci_test_start(&x, "-device usb-kbd");
    ring = xhci_configure_ep(&x, 3, EP_TYPE_INTR_IN, interval, 8);
    data = xhci_alloc_page(&x);

    qtest_clock_step(x.qts, ((1ULL << 32) + 5) * XHCI_MICROFRAME_NS);
    /* A ready report makes an early transfer observable. */
    qtest_qmp_assert_success(x.qts,
        "{'execute': 'input-send-event', 'arguments': {'events': ["
        "{'type': 'key', 'data': {'down': true, "
        "'key': {'type': 'qcode', 'data': 'a'}}}]}}");

    xhci_write_trb(&x, ring, data, sizeof(report),
                   TRB_TYPE(TR_NORMAL) | TRB_TR_IOC | TRB_C);
    xhci_writel(&x, x.doorbell + 4 * x.slot, 3);
    g_assert_false(xhci_next_event(&x, NULL, NULL));

    qtest_clock_step(x.qts,
                     ((1U << interval) - 5) * XHCI_MICROFRAME_NS - 1);
    g_assert_false(xhci_next_event(&x, NULL, NULL));
    qtest_clock_step(x.qts, 1);
    g_assert_true(xhci_next_event(&x, &status, &control));
    g_assert_cmpuint(TRB_GET_TYPE(control), ==, ER_TRANSFER);
    g_assert_cmpuint(TRB_GET_CCODE(status), ==, CC_SUCCESS);
    qtest_memread(x.qts, data, report, sizeof(report));
    g_assert_cmpmem(report, sizeof(report), expected, sizeof(expected));

    xhci_test_end(&x);
}

/*
 * The endpoint type in the endpoint context is whatever the guest says. Tell
 * the controller that the interrupt endpoint of usb-kbd is isoch. The idle
 * keyboard NAKs, and the TD has to stay pending when first the kick timer and
 * then a doorbell retry it.
 */
static void test_xhci_isoch_ep_type_mismatch(void)
{
    uint64_t ring;
    XHCITest x;

    if (!xhci_test_supported("usb-kbd")) {
        return;
    }

    xhci_test_start(&x, "-device usb-kbd");
    ring = xhci_configure_ep(&x, 3, EP_TYPE_ISOCH_IN, 0, 8);

    xhci_write_trb(&x, ring, xhci_alloc_page(&x), 8,
                   TRB_TYPE(TR_ISOCH) | TRB_TR_SIA | TRB_TR_IOC | TRB_C);
    xhci_writel(&x, x.doorbell + 4 * x.slot, 3);
    qtest_clock_step(x.qts, 2 * XHCI_MICROFRAME_NS);
    xhci_writel(&x, x.doorbell + 4 * x.slot, 3);

    g_assert_false(xhci_next_event(&x, NULL, NULL));
    g_assert_cmphex(xhci_readl(&x, x.oper + XHCI_USBSTS) &
                    (USBSTS_HCH | USBSTS_HCE), ==, 0);

    xhci_test_end(&x);
}

/*
 * A TD's Frame ID holds the low 11 bits of its frame. One behind the current
 * frame is late and runs at once, one ahead waits for its frame, across the
 * 2048-frame wrap as well. A TD one frame late just after the wrap, or 100
 * frames late, used to wait 2048 frames.
 */
static void test_xhci_isoch_late_frame_id(void)
{
    static const struct {
        unsigned int now, frame, wait;  /* frames */
    } cases[] = {
        { 0, 2047, 0 },
        { 1000, 900, 0 },
        { 1000, 1100, 100 },
        { 2000, 50, 98 },
    };
    uint32_t status, control, mfindex;
    uint64_t ring;
    XHCITest x;
    int i;

    if (!xhci_test_supported("usb-audio")) {
        return;
    }

    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        xhci_test_start(&x, "-audiodev none,id=snd0 "
                            "-device usb-audio,audiodev=snd0");
        ring = xhci_configure_ep(&x, 2, EP_TYPE_ISOCH_OUT, 0, 64);

        /* to microframe 1 of frame cases[i].now in the next window */
        mfindex = xhci_readl(&x, x.runtime + XHCI_MFINDEX) & 0x3fff;
        qtest_clock_step(x.qts, (0x4000 - mfindex + (cases[i].now << 3) + 1) *
                         XHCI_MICROFRAME_NS);
        g_assert_cmpuint(xhci_readl(&x, x.runtime + XHCI_MFINDEX), ==,
                         (cases[i].now << 3) + 1);

        /* usb-audio stalls the TD: its streaming interface is not enabled */
        xhci_write_trb(&x, ring, xhci_alloc_page(&x), 64,
                       TRB_TYPE(TR_ISOCH) | TRB_TR_FRAMEID(cases[i].frame) |
                       TRB_TR_IOC | TRB_C);
        xhci_writel(&x, x.doorbell + 4 * x.slot, 2);
        if (cases[i].wait) {
            qtest_clock_step(x.qts, ((cases[i].wait << 3) - 2) *
                             XHCI_MICROFRAME_NS);
            g_assert_false(xhci_next_event(&x, NULL, NULL));
        }
        qtest_clock_step(x.qts, 2 * XHCI_MICROFRAME_NS);
        g_assert_true(xhci_next_event(&x, &status, &control));
        g_assert_cmpuint(TRB_GET_TYPE(control), ==, ER_TRANSFER);
        g_assert_cmpuint(TRB_GET_CCODE(status), ==, CC_STALL_ERROR);

        xhci_test_end(&x);
    }
}

/*
 * An isoch ring found empty is reported once (Ring Underrun for OUT), not at
 * each doorbell rung on it: macOS rings the doorbell again on the event, and
 * the two fed each other until the event ring filled. A TD run makes the next
 * empty ring a new condition, reported once again.
 */
static void test_xhci_isoch_underrun_once(void)
{
    uint32_t status, control;
    uint64_t ring;
    XHCITest x;
    int i;

    if (!xhci_test_supported("usb-audio")) {
        return;
    }

    xhci_test_start(&x, "-audiodev none,id=snd0 "
                        "-device usb-audio,audiodev=snd0");
    ring = xhci_configure_ep(&x, 2, EP_TYPE_ISOCH_OUT, 0, 64);

    for (i = 0; i < 3; i++) {
        xhci_writel(&x, x.doorbell + 4 * x.slot, 2);
        qtest_clock_step(x.qts, XHCI_MICROFRAME_NS);
    }
    g_assert_true(xhci_next_event(&x, &status, &control));
    g_assert_cmpuint(TRB_GET_TYPE(control), ==, ER_TRANSFER);
    g_assert_cmpuint(TRB_GET_CCODE(status), ==, CC_RING_UNDERRUN);
    g_assert_false(xhci_next_event(&x, NULL, NULL));

    /* usb-audio stalls the TD: its streaming interface is not enabled */
    xhci_write_trb(&x, ring, xhci_alloc_page(&x), 64,
                   TRB_TYPE(TR_ISOCH) | TRB_TR_SIA | TRB_TR_IOC | TRB_C);
    xhci_writel(&x, x.doorbell + 4 * x.slot, 2);
    qtest_clock_step(x.qts, 8 * XHCI_MICROFRAME_NS);
    g_assert_true(xhci_next_event(&x, &status, &control));
    g_assert_cmpuint(TRB_GET_TYPE(control), ==, ER_TRANSFER);
    g_assert_cmpuint(TRB_GET_CCODE(status), ==, CC_STALL_ERROR);
    g_assert_true(xhci_next_event(&x, &status, &control));
    g_assert_cmpuint(TRB_GET_TYPE(control), ==, ER_TRANSFER);
    g_assert_cmpuint(TRB_GET_CCODE(status), ==, CC_RING_UNDERRUN);

    xhci_writel(&x, x.doorbell + 4 * x.slot, 2);
    qtest_clock_step(x.qts, XHCI_MICROFRAME_NS);
    g_assert_false(xhci_next_event(&x, NULL, NULL));
    g_assert_cmphex(xhci_readl(&x, x.oper + XHCI_USBSTS) &
                    (USBSTS_HCH | USBSTS_HCE), ==, 0);

    xhci_test_end(&x);
}

static unsigned int xhci_ep_state(XHCITest *x, unsigned int epid)
{
    return qtest_readl(x->qts, x->dev_ctx + 0x20 * epid) & EP_STATE_MASK;
}

/*
 * Only a doorbell restarts a stopped endpoint. A key pressed after Stop
 * Endpoint wakes the interrupt endpoint of usb-kbd, which has to stay
 * stopped, its TD not run, until the doorbell: macOS reads the endpoint
 * context once the command completes, and gives up on an endpoint it finds
 * running again.
 */
static void test_xhci_stopped_ep_wakeup(void)
{
    uint32_t status, control;
    uint64_t ring;
    XHCITest x;

    if (!xhci_test_supported("usb-kbd")) {
        return;
    }

    xhci_test_start(&x, "-device usb-kbd");
    ring = xhci_configure_ep(&x, 3, EP_TYPE_INTR_IN, 3, 8);
    xhci_writel(&x, x.doorbell + 4 * x.slot, 3);
    xhci_command(&x, 0, TRB_TYPE(CR_STOP_ENDPOINT) | (3 << 16) |
                 (x.slot << 24));
    g_assert_cmpuint(xhci_ep_state(&x, 3), ==, EP_STATE_STOPPED);

    xhci_write_trb(&x, ring, xhci_alloc_page(&x), 8,
                   TRB_TYPE(TR_NORMAL) | TRB_TR_IOC | TRB_C);
    qtest_qmp_assert_success(x.qts, "{'execute': 'send-key', 'arguments': "
                             "{'keys': [{'type': 'qcode', 'data': 'a'}]}}");
    qtest_clock_step(x.qts, 16 * XHCI_MICROFRAME_NS);
    g_assert_false(xhci_next_event(&x, NULL, NULL));
    g_assert_cmpuint(xhci_ep_state(&x, 3), ==, EP_STATE_STOPPED);

    xhci_writel(&x, x.doorbell + 4 * x.slot, 3);
    qtest_clock_step(x.qts, 16 * XHCI_MICROFRAME_NS);
    g_assert_true(xhci_next_event(&x, &status, &control));
    g_assert_cmpuint(TRB_GET_TYPE(control), ==, ER_TRANSFER);
    g_assert_cmpuint(TRB_GET_CCODE(status), ==, CC_SUCCESS);

    xhci_test_end(&x);
}

/*
 * A TD with Block Event Interrupt set completes with its event written and
 * no interrupt asserted; the next TD without it asserts the interrupt.
 * usb-kbd completes a TD on its interrupt endpoint when a key goes down and
 * when it comes up again.
 */
static void test_xhci_bei(void)
{
    uint32_t status, control;
    uint64_t ring;
    XHCITest x;

    if (!xhci_test_supported("usb-kbd")) {
        return;
    }

    xhci_test_start(&x, "-device usb-kbd");
    ring = xhci_configure_ep(&x, 3, EP_TYPE_INTR_IN, 3, 8);
    xhci_writel(&x, x.runtime + XHCI_IMAN, IMAN_IP);
    g_assert_cmphex(xhci_readl(&x, x.runtime + XHCI_IMAN) & IMAN_IP, ==, 0);

    xhci_write_trb(&x, ring, xhci_alloc_page(&x), 8,
                   TRB_TYPE(TR_NORMAL) | TRB_TR_IOC | TRB_TR_BEI | TRB_C);
    xhci_write_trb(&x, ring + 16, xhci_alloc_page(&x), 8,
                   TRB_TYPE(TR_NORMAL) | TRB_TR_IOC | TRB_C);
    xhci_writel(&x, x.doorbell + 4 * x.slot, 3);
    qtest_clock_step(x.qts, 16 * XHCI_MICROFRAME_NS);
    g_assert_false(xhci_next_event(&x, NULL, NULL));

    /* the key goes down: the first TD completes */
    qtest_qmp_assert_success(x.qts, "{'execute': 'send-key', 'arguments': "
                             "{'keys': [{'type': 'qcode', 'data': 'a'}]}}");
    qtest_clock_step(x.qts, 16 * XHCI_MICROFRAME_NS);
    g_assert_cmphex(xhci_readl(&x, x.runtime + XHCI_IMAN) & IMAN_IP, ==, 0);
    g_assert_true(xhci_next_event(&x, &status, &control));
    g_assert_cmpuint(TRB_GET_TYPE(control), ==, ER_TRANSFER);
    g_assert_cmpuint(TRB_GET_CCODE(status), ==, CC_SUCCESS);
    g_assert_false(xhci_next_event(&x, NULL, NULL));

    /* send-key releases the key 100 ms later: the second TD completes */
    qtest_clock_step(x.qts, 200 * 1000 * 1000);
    g_assert_cmphex(xhci_readl(&x, x.runtime + XHCI_IMAN) & IMAN_IP, ==,
                    IMAN_IP);
    g_assert_true(xhci_next_event(&x, &status, &control));
    g_assert_cmpuint(TRB_GET_TYPE(control), ==, ER_TRANSFER);
    g_assert_cmpuint(TRB_GET_CCODE(status), ==, CC_SUCCESS);

    xhci_test_end(&x);
}

/* Queue a command and ring the doorbell, leaving its event on the ring */
static void xhci_post_command(XHCITest *x, uint32_t control)
{
    g_assert_cmpuint(x->cmd_idx, <, XHCI_RING_TRBS);
    xhci_write_trb(x, x->cmd_ring + 16 * x->cmd_idx++, 0, 0, control | TRB_C);
    xhci_writel(x, x->doorbell, 0);
}

static bool xhci_ip(XHCITest *x)
{
    return xhci_readl(x, x->runtime + XHCI_IMAN) & IMAN_IP;
}

static void xhci_clear_ip(XHCITest *x)
{
    xhci_writel(x, x->runtime + XHCI_IMAN, IMAN_IP);
    g_assert_false(xhci_ip(x));
}

/*
 * Fetch the next event, which has to be a Transfer Event, and return its
 * TRB pointer. Does not advance the clock.
 */
static uint64_t xhci_transfer_event(XHCITest *x, uint32_t *status)
{
    uint64_t trb = qtest_readq(x->qts, x->event_ring + 16 * x->event_idx);
    uint32_t control;

    g_assert_true(xhci_next_event(x, status, &control));
    g_assert_cmpuint(TRB_GET_TYPE(control), ==, ER_TRANSFER);
    return trb;
}

/*
 * A Short Packet ends a TD's data on the TRB it meets, which reports it if
 * it has ISP or IOC, and each later TRB of the TD with IOC reports it again
 * with the same residue (xHCI 4.10.1.1.2): software takes the TD as
 * complete only at the event of its last TRB. macOS queues an isoch IN TD
 * that crosses a page as an Isoch TRB with ISP chained to a Normal TRB with
 * ISP and IOC, and waits for the second event. Without it, it took each
 * later event of the endpoint as mismatched and a camera's stream stopped.
 * The idle usb-kbd NAKs a TD on its interrupt endpoint, and answers it with
 * an 8-byte report when a key goes down; typed isoch, the endpoint takes an
 * Isoch TRB, and its ring, empty after the TD, is reported.
 */
static void test_xhci_short_packet(void)
{
    static const struct {
        unsigned int ep_type, interval;
        uint32_t len[2], control[2];
        bool ip;
        struct {
            unsigned int trb, ccode, length;
        } events[2];
        unsigned int nr_events;
    } cases[] = {
        /* ISP on the first TRB and IOC on the last: two events, no IRQ */
        { EP_TYPE_INTR_IN, 3, { 16, 8 },
          { TRB_TYPE(TR_NORMAL) | TRB_TR_ISP | TRB_TR_CH | TRB_TR_BEI,
            TRB_TYPE(TR_NORMAL) | TRB_TR_ISP | TRB_TR_IOC | TRB_TR_BEI },
          false, { { 0, CC_SHORT_PACKET, 8 }, { 1, CC_SHORT_PACKET, 8 } }, 2 },
        /* ISP and IOC on the first TRB: one event for it, one for the last */
        { EP_TYPE_INTR_IN, 3, { 16, 8 },
          { TRB_TYPE(TR_NORMAL) | TRB_TR_ISP | TRB_TR_IOC | TRB_TR_CH,
            TRB_TYPE(TR_NORMAL) | TRB_TR_IOC },
          true, { { 0, CC_SHORT_PACKET, 8 }, { 1, CC_SHORT_PACKET, 8 } }, 2 },
        /* IOC on a TRB filled before the short one */
        { EP_TYPE_INTR_IN, 3, { 8, 8 },
          { TRB_TYPE(TR_NORMAL) | TRB_TR_IOC | TRB_TR_CH,
            TRB_TYPE(TR_NORMAL) | TRB_TR_ISP | TRB_TR_IOC },
          true, { { 0, CC_SUCCESS, 0 }, { 1, CC_SHORT_PACKET, 8 } }, 2 },
        /* no Short Packet: the last TRB's event alone */
        { EP_TYPE_INTR_IN, 3, { 4, 4 },
          { TRB_TYPE(TR_NORMAL) | TRB_TR_ISP | TRB_TR_CH,
            TRB_TYPE(TR_NORMAL) | TRB_TR_ISP | TRB_TR_IOC },
          true, { { 1, CC_SUCCESS, 0 } }, 1 },
        /* macOS's isoch IN TD */
        { EP_TYPE_ISOCH_IN, 0, { 16, 8 },
          { TRB_TYPE(TR_ISOCH) | TRB_TR_SIA | TRB_TR_ISP | TRB_TR_CH |
            TRB_TR_BEI,
            TRB_TYPE(TR_NORMAL) | TRB_TR_ISP | TRB_TR_IOC | TRB_TR_BEI },
          true, { { 0, CC_SHORT_PACKET, 8 }, { 1, CC_SHORT_PACKET, 8 } }, 2 },
    };
    uint32_t status;
    uint64_t ring;
    XHCITest x;
    int i, j;

    if (!xhci_test_supported("usb-kbd")) {
        return;
    }

    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        xhci_test_start(&x, "-device usb-kbd");
        ring = xhci_configure_ep(&x, 3, cases[i].ep_type, cases[i].interval,
                                 8);
        for (j = 0; j < 2; j++) {
            xhci_write_trb(&x, ring + 16 * j, xhci_alloc_page(&x),
                           cases[i].len[j], cases[i].control[j] | TRB_C);
        }
        xhci_writel(&x, x.doorbell + 4 * x.slot, 3);
        qtest_clock_step(x.qts, 16 * XHCI_MICROFRAME_NS);
        g_assert_false(xhci_next_event(&x, NULL, NULL));

        /* the key goes down: the TD completes */
        xhci_clear_ip(&x);
        qtest_qmp_assert_success(x.qts,
            "{'execute': 'send-key', 'arguments': "
            "{'keys': [{'type': 'qcode', 'data': 'a'}]}}");
        qtest_clock_step(x.qts, 16 * XHCI_MICROFRAME_NS);

        g_assert_cmpuint(xhci_ip(&x), ==, cases[i].ip);
        for (j = 0; j < cases[i].nr_events; j++) {
            g_assert_cmphex(xhci_transfer_event(&x, &status), ==,
                            ring + 16 * cases[i].events[j].trb);
            g_assert_cmpuint(TRB_GET_CCODE(status), ==,
                             cases[i].events[j].ccode);
            g_assert_cmpuint(TRB_GET_PARAM(status), ==,
                             cases[i].events[j].length);
        }
        if (cases[i].ep_type == EP_TYPE_ISOCH_IN) {
            g_assert_cmphex(xhci_transfer_event(&x, &status), ==, ring + 32);
            g_assert_cmpuint(TRB_GET_CCODE(status), ==, CC_RING_OVERRUN);
        }
        g_assert_false(xhci_next_event(&x, NULL, NULL));

        xhci_test_end(&x);
    }
}

/*
 * Interrupt moderation (xHCI 4.17.2): IMODI defaults to 4000 (1 ms). Once
 * the interrupter asserts, IMODC counts IMODI down, and an event posted
 * meanwhile asserts it only when IMODC reaches 0. An event posted after the
 * count ran out asserts it at once, as every event does with IMODI at 0.
 */
static void test_xhci_imod(void)
{
    XHCITest x;
    uint32_t imod;

    if (!xhci_test_supported("usb-kbd")) {
        return;
    }

    xhci_test_start(&x, "-device usb-kbd");
    g_assert_cmphex(xhci_readl(&x, x.runtime + XHCI_IMOD) & 0xffff, ==, 4000);

    /* 250 us, and IMODC at 0 */
    xhci_writel(&x, x.runtime + XHCI_IMOD, 1000);
    xhci_clear_ip(&x);
    xhci_post_command(&x, TRB_TYPE(CR_NOOP));
    g_assert_true(xhci_ip(&x));
    imod = xhci_readl(&x, x.runtime + XHCI_IMOD);
    g_assert_cmpuint(imod & 0xffff, ==, 1000);
    g_assert_cmpuint(imod >> 16, ==, 1000);
    g_assert_true(xhci_next_event(&x, NULL, NULL));
    xhci_clear_ip(&x);

    /* moderated until IMODC reaches 0 */
    xhci_post_command(&x, TRB_TYPE(CR_NOOP));
    g_assert_false(xhci_ip(&x));
    qtest_clock_step(x.qts, 100 * 1000);
    g_assert_false(xhci_ip(&x));
    g_assert_cmpuint(xhci_readl(&x, x.runtime + XHCI_IMOD) >> 16, ==, 600);
    qtest_clock_step(x.qts, 150 * 1000);
    g_assert_true(xhci_ip(&x));
    g_assert_true(xhci_next_event(&x, NULL, NULL));
    xhci_clear_ip(&x);

    /* the count runs out with no event waiting: the next one asserts at once */
    qtest_clock_step(x.qts, 1000 * 1000);
    g_assert_cmpuint(xhci_readl(&x, x.runtime + XHCI_IMOD) >> 16, ==, 0);
    xhci_post_command(&x, TRB_TYPE(CR_NOOP));
    g_assert_true(xhci_ip(&x));
    g_assert_true(xhci_next_event(&x, NULL, NULL));
    xhci_clear_ip(&x);

    /* IMODI 0: no moderation */
    xhci_writel(&x, x.runtime + XHCI_IMOD, 0);
    xhci_post_command(&x, TRB_TYPE(CR_NOOP));
    g_assert_true(xhci_ip(&x));
    g_assert_true(xhci_next_event(&x, NULL, NULL));
    xhci_clear_ip(&x);
    xhci_post_command(&x, TRB_TYPE(CR_NOOP));
    g_assert_true(xhci_ip(&x));
    g_assert_true(xhci_next_event(&x, NULL, NULL));

    xhci_test_end(&x);
}

/*
 * Set Latency Tolerance Value (xHCI 4.6.14) completes with Success on an xHC
 * with LTC set, which qemu-xhci has on PCI Express, and with TRB Error on one
 * without it, which it is on conventional PCI.
 */
static void test_xhci_set_ltv(void)
{
    static const struct {
        const char *machine;
        bool ltc;
    } cases[] = {
        { "q35", true },
        { "pc", false },
    };
    uint32_t status, control;
    XHCITest x;
    int i;

    if (!xhci_test_supported("usb-kbd")) {
        return;
    }

    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        xhci_test_start_on(&x, cases[i].machine, "-device usb-kbd");
        g_assert_cmphex(xhci_readl(&x, XHCI_HCCPARAMS1) & HCCP1_LTC, ==,
                        cases[i].ltc ? HCCP1_LTC : 0);
        xhci_post_command(&x, TRB_TYPE(CR_SET_LATENCY_TOLERANCE) |
                          (0x401 << 16));
        g_assert_true(xhci_next_event(&x, &status, &control));
        g_assert_cmpuint(TRB_GET_TYPE(control), ==, ER_COMMAND_COMPLETE);
        g_assert_cmpuint(TRB_GET_CCODE(status), ==,
                         cases[i].ltc ? CC_SUCCESS : CC_TRB_ERROR);
        g_assert_cmpuint(TRB_GET_SLOT(control), ==, 0);
        xhci_test_end(&x);
    }
}

typedef struct XHCICommandCase {
    uint64_t parameter;
    uint32_t control;
    /* what its Command Completion Event TRB holds */
    unsigned int type;
    unsigned int ccode;
    uint32_t param;     /* Command Completion Parameter */
    unsigned int vf;
    unsigned int slot;
    bool vendor;        /* a vendor defined reply: check its type alone */
} XHCICommandCase;

/*
 * Queue cases[] on the Command Ring, ring its doorbell once, and check that
 * each command's completion holds what that command alone gives it.
 */
static void xhci_check_commands(XHCITest *x, const XHCICommandCase *cases,
                                int n)
{
    uint64_t trb[8];
    uint32_t status, control;
    int i;

    g_assert_cmpint(n, <=, ARRAY_SIZE(trb));
    g_assert_cmpuint(x->cmd_idx + n, <=, XHCI_RING_TRBS);
    for (i = 0; i < n; i++) {
        trb[i] = x->cmd_ring + 16 * x->cmd_idx++;
        xhci_write_trb(x, trb[i], cases[i].parameter, 0,
                       cases[i].control | TRB_C);
    }
    xhci_writel(x, x->doorbell, 0);

    for (i = 0; i < n; i++) {
        uint64_t ptr = qtest_readq(x->qts, x->event_ring + 16 * x->event_idx);

        g_assert_true(xhci_next_event(x, &status, &control));
        g_assert_cmphex(ptr, ==, trb[i]);
        g_assert_cmpuint(TRB_GET_TYPE(control), ==, cases[i].type);
        if (cases[i].vendor) {
            continue;
        }
        g_assert_cmpuint(TRB_GET_CCODE(status), ==, cases[i].ccode);
        g_assert_cmphex(TRB_GET_PARAM(status), ==, cases[i].param);
        g_assert_cmpuint(TRB_GET_VF(control), ==, cases[i].vf);
        g_assert_cmpuint(TRB_GET_SLOT(control), ==, cases[i].slot);
    }
    g_assert_false(xhci_next_event(x, NULL, NULL));
}

/*
 * Commands taken at one doorbell complete with their own fields (xHCI
 * 6.4.2.2): a command that is not for a slot has Slot ID 0 after one that
 * is, an NEC reply has Success after a command that failed, and a Command
 * Completion Event after an NEC reply holds none of the reply's fields.
 */
static void test_xhci_command_completion_fields(void)
{
    XHCITest x;
    uint64_t bw, deq;
    uint32_t ep0;

    if (!xhci_test_supported("usb-kbd") ||
        !qtest_has_device("nec-usb-xhci")) {
        return;
    }

    xhci_test_start(&x, "-device usb-kbd");
    bw = xhci_alloc_page(&x);
    deq = xhci_alloc_page(&x) | 1;
    ep0 = TRB_CR_SLOT(x.slot) | TRB_CR_EPID(1);
    {
        const XHCICommandCase cases[] = {
            { 0, TRB_TYPE(CR_STOP_ENDPOINT) | ep0,
              ER_COMMAND_COMPLETE, CC_SUCCESS, 0, 0, x.slot },
            { 0, TRB_TYPE(CR_NOOP), ER_COMMAND_COMPLETE, CC_SUCCESS },
            { deq, TRB_TYPE(CR_SET_TR_DEQUEUE) | ep0,
              ER_COMMAND_COMPLETE, CC_SUCCESS, 0, 0, x.slot },
            { bw, TRB_TYPE(CR_GET_PORT_BANDWIDTH) |
              TRB_CR_DEV_SPEED(SPEED_ID_HIGH),
              ER_COMMAND_COMPLETE, CC_SUCCESS },
            { deq, TRB_TYPE(CR_SET_TR_DEQUEUE) | ep0,
              ER_COMMAND_COMPLETE, CC_SUCCESS, 0, 0, x.slot },
            /* no NEC commands on qemu-xhci */
            { 0, TRB_TYPE(CR_NEC_FIRMWARE_REVISION),
              ER_COMMAND_COMPLETE, CC_TRB_ERROR },
            { deq, TRB_TYPE(CR_SET_TR_DEQUEUE) | ep0,
              ER_COMMAND_COMPLETE, CC_SUCCESS, 0, 0, x.slot },
            /* a command it does not support */
            { 0, TRB_TYPE(CR_FORCE_HEADER),
              ER_COMMAND_COMPLETE, CC_TRB_ERROR },
        };

        xhci_check_commands(&x, cases, ARRAY_SIZE(cases));
    }
    xhci_test_end(&x);

    xhci_test_start_hc(&x, "pc", "nec-usb-xhci", "-device usb-kbd");
    {
        const XHCICommandCase cases[] = {
            { 0, TRB_TYPE(CR_NEC_FIRMWARE_REVISION),
              ER_NEC_REPLY, CC_SUCCESS, 0x3034 },
            { 0, TRB_TYPE(CR_NOOP), ER_COMMAND_COMPLETE, CC_SUCCESS },
            /* slot 5 is not enabled */
            { 0, TRB_TYPE(CR_STOP_ENDPOINT) | TRB_CR_SLOT(5) | TRB_CR_EPID(1),
              ER_COMMAND_COMPLETE, CC_SLOT_NOT_ENABLED, 0, 0, 5 },
            { 0, TRB_TYPE(CR_NEC_FIRMWARE_REVISION),
              ER_NEC_REPLY, CC_SUCCESS, 0x3034 },
            /* its reply's Slot ID and VF ID fields are not 0 */
            { 0x0102030405060708ULL, TRB_TYPE(CR_NEC_CHALLENGE_RESPONSE),
              ER_NEC_REPLY, .vendor = true },
            { 0, TRB_TYPE(CR_NOOP), ER_COMMAND_COMPLETE, CC_SUCCESS },
        };

        xhci_check_commands(&x, cases, ARRAY_SIZE(cases));
    }
    xhci_test_end(&x);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/xhci/pci/hotplug", test_xhci_hotplug);
    qtest_add_func("/xhci/pci/unplug/finalize", test_xhci_unplug_finalize);
    if (qtest_has_device("usb-uas")) {
        qtest_add_func("/xhci/pci/hotplug/usb-uas", test_usb_uas_hotplug);
    }
    if (qtest_has_device("usb-ccid")) {
        qtest_add_func("/xhci/pci/hotplug/usb-ccid", test_usb_ccid_hotplug);
    }
    qtest_add_func("/xhci/pci/address-device/dcbaa-read-error",
                   test_xhci_address_device_dcbaa_read_error);
    qtest_add_func("/xhci/pci/isoch/mfindex-32bit",
                   test_xhci_isoch_mfindex_32bit);
    qtest_add_func("/xhci/pci/intr/mfindex-32bit",
                   test_xhci_intr_mfindex_32bit);
    qtest_add_func("/xhci/pci/isoch/ep-type-mismatch",
                   test_xhci_isoch_ep_type_mismatch);
    qtest_add_func("/xhci/pci/isoch/underrun-once",
                   test_xhci_isoch_underrun_once);
    qtest_add_func("/xhci/pci/isoch/late-frame-id",
                   test_xhci_isoch_late_frame_id);
    qtest_add_func("/xhci/pci/stopped-ep-wakeup",
                   test_xhci_stopped_ep_wakeup);
    qtest_add_func("/xhci/pci/bei", test_xhci_bei);
    qtest_add_func("/xhci/pci/short-packet", test_xhci_short_packet);
    qtest_add_func("/xhci/pci/imod", test_xhci_imod);
    qtest_add_func("/xhci/pci/set-ltv", test_xhci_set_ltv);
    qtest_add_func("/xhci/pci/command-completion-fields",
                   test_xhci_command_completion_fields);

    qtest_start("-device nec-usb-xhci,id=xhci"
                " -drive id=drive0,if=none,file=null-co://,"
                "file.read-zeroes=on,format=raw");
    ret = g_test_run();
    qtest_end();

    return ret;
}
