/*
 * QTest testcase for the virtio PCI devices' x-ascending-capabilities
 *
 * macOS 27's virtio PCI transport revisits PCI_CFG and NOTIFY forever on a
 * capability list whose links go down in address, so vmapple turns the
 * property on for virtio-net-pci and virtio-sound-pci: each capability keeps
 * its contents and offset, and the list links them in address order.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/pci-pc.h"
#include "hw/pci/pci_regs.h"

#define CAPS_MAX 48

typedef struct {
    const char *name;
    const char *options;
} CapsDevice;

/* The offsets of the device's capabilities at 05.0, in its list's order. */
static int caps(const CapsDevice *d, bool ascending, uint8_t *offsets)
{
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    uint8_t offset;
    int n = 0;

    qts = qtest_initf("-nodefaults -audiodev none,id=snd "
                      "-device %s%s,addr=05.0,x-ascending-capabilities=%s",
                      d->name, d->options, ascending ? "on" : "off");
    bus = qpci_new_pc(qts, NULL);
    dev = qpci_device_find(bus, QPCI_DEVFN(0x5, 0x0));
    g_assert(dev);
    g_assert(qpci_config_readw(dev, PCI_STATUS) & PCI_STATUS_CAP_LIST);
    for (offset = qpci_config_readb(dev, PCI_CAPABILITY_LIST); offset;
         offset = qpci_config_readb(dev, offset + PCI_CAP_LIST_NEXT)) {
        g_assert_cmpint(n, <, CAPS_MAX);
        offsets[n++] = offset;
    }
    g_free(dev);
    qpci_free_pc(bus);
    qtest_quit(qts);
    return n;
}

/*
 * With the property on, the list holds the capabilities it holds with it
 * off, each at its own offset, linked in address order.
 */
static void test_ascending(gconstpointer data)
{
    const CapsDevice *d = data;
    uint8_t on[CAPS_MAX], off[CAPS_MAX];
    int n_on, n_off, i, j;

    if (!qtest_has_device(d->name)) {
        g_test_skip("the device is not built");
        return;
    }
    n_on = caps(d, true, on);
    n_off = caps(d, false, off);
    g_assert_cmpint(n_on, >, 1);
    g_assert_cmpint(n_on, ==, n_off);
    for (i = 0; i < n_on; i++) {
        if (i) {
            g_assert_cmphex(on[i - 1], <, on[i]);
        }
        for (j = 0; j < n_off && off[j] != on[i]; j++) {
        }
        g_assert_cmpint(j, <, n_off);
    }
}

int main(int argc, char **argv)
{
    static const CapsDevice sound = { "virtio-sound-pci", ",audiodev=snd" };
    static const CapsDevice net = { "virtio-net-pci", "" };

    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("/virtio-pci-caps/sound/ascending", &sound,
                        test_ascending);
    qtest_add_data_func("/virtio-pci-caps/net/ascending", &net,
                        test_ascending);
    return g_test_run();
}
