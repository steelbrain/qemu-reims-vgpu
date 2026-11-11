/*
 * QTest testcase for screendump: a PPM has the display's pixels, row for
 * row, at widths whose rows of 3-byte pixels end off a 4-byte boundary
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/pci-pc.h"
#include "hw/display/bochs-vbe.h"

#define HEIGHT 64

/* Channel c (0 B, 1 G, 2 R) of the pixel at (x, y): differs along rows. */
static uint8_t pattern(int x, int y, int c)
{
    return (x * 7 + y * 131 + c * 61) & 0xff;
}

static void dispi_write(QPCIDevice *dev, QPCIBar mmio, int index,
                        uint16_t value)
{
    qpci_io_writew(dev, mmio, PCI_VGA_BOCHS_OFFSET + index * 2, value);
}

/*
 * bochs-display shows WIDTH x HEIGHT pixels of 32 bits; QMP's screendump
 * writes them as a PPM, which has WIDTH * 3 bytes per row and no padding.
 */
static void test_ppm(gconstpointer data)
{
    int width = GPOINTER_TO_INT(data);
    g_autofree char *path = NULL;
    g_autofree uint8_t *pixels = g_malloc(width * HEIGHT * 4);
    g_autofree char *header = g_strdup_printf("P6\n%d %d\n255\n",
                                              width, HEIGHT);
    g_autofree char *ppm = NULL;
    size_t header_len = strlen(header);
    gsize len;
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar fb, mmio;
    int fd, x, y, c;

    fd = g_file_open_tmp("screendump-XXXXXX.ppm", &path, NULL);
    g_assert(fd >= 0);
    close(fd);

    qts = qtest_init("-vga none -device bochs-display,addr=04.0");
    bus = qpci_new_pc(qts, NULL);
    dev = qpci_device_find(bus, QPCI_DEVFN(0x4, 0x0));
    g_assert(dev != NULL);
    qpci_device_enable(dev);
    fb = qpci_iomap(dev, 0, NULL);
    mmio = qpci_iomap(dev, 2, NULL);

    /* Little-endian x8r8g8b8: B, G, R, X in memory. */
    for (y = 0; y < HEIGHT; y++) {
        for (x = 0; x < width; x++) {
            for (c = 0; c < 3; c++) {
                pixels[(y * width + x) * 4 + c] = pattern(x, y, c);
            }
            pixels[(y * width + x) * 4 + 3] = 0;
        }
    }
    qpci_memwrite(dev, fb, 0, pixels, width * HEIGHT * 4);

    dispi_write(dev, mmio, VBE_DISPI_INDEX_XRES, width);
    dispi_write(dev, mmio, VBE_DISPI_INDEX_YRES, HEIGHT);
    dispi_write(dev, mmio, VBE_DISPI_INDEX_BPP, 32);
    dispi_write(dev, mmio, VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED);

    qtest_qmp_assert_success(qts, "{'execute': 'screendump', "
                             "'arguments': {'filename': %s}}", path);

    g_assert(g_file_get_contents(path, &ppm, &len, NULL));
    g_assert_cmpuint(len, ==, header_len + width * HEIGHT * 3);
    g_assert_cmpmem(ppm, header_len, header, header_len);
    for (y = 0; y < HEIGHT; y++) {
        for (x = 0; x < width; x++) {
            const uint8_t *p = (uint8_t *)ppm + header_len +
                               (y * width + x) * 3;
            g_assert_cmpuint(p[0], ==, pattern(x, y, 2));
            g_assert_cmpuint(p[1], ==, pattern(x, y, 1));
            g_assert_cmpuint(p[2], ==, pattern(x, y, 0));
        }
    }

    unlink(path);
    g_free(dev);
    qpci_free_pc(bus);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    /* 66 * 3 bytes end 2 bytes off a 4-byte boundary, 67 * 3 bytes 1. */
    static const int widths[] = { 64, 66, 67 };

    g_test_init(&argc, &argv, NULL);

    if (qtest_has_device("bochs-display")) {
        for (int i = 0; i < ARRAY_SIZE(widths); i++) {
            g_autofree char *testpath =
                g_strdup_printf("/display/screendump/ppm/%d", widths[i]);
            qtest_add_data_func(testpath, GINT_TO_POINTER(widths[i]),
                                test_ppm);
        }
    }

    return g_test_run();
}
