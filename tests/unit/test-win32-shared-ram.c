/*
 * Windows file-backed guest RAM ownership contracts.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/os-win32.h"
#include <io.h>

static void test_alias_lifetime(void)
{
    Error *error = NULL;
    int fd = -1;
    uint8_t *memory, *alias;
    HANDLE section;

    memory = qemu_win32_shared_ram_alloc(8192, &fd, &error);
    g_assert_null(error);
    g_assert_nonnull(memory);
    g_assert_cmpint(fd, >=, 0);
    section = CreateFileMappingW((HANDLE)_get_osfhandle(fd), NULL,
                                  PAGE_READWRITE, 0, 0, NULL);
    g_assert_nonnull(section);
    alias = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 8192);
    g_assert_nonnull(alias);

    /* The GPU's independent view sees the same pages in both directions. */
    g_assert_cmpuint(alias[4096], ==, 0);
    memory[4096] = 0x37;
    g_assert_cmpuint(alias[4096], ==, 0x37);
    alias[8191] = 0x5a;
    g_assert_cmpuint(memory[8191], ==, 0x5a);

    /* A live section view retains the file after QEMU closes its own view/fd. */
    qemu_win32_shared_ram_free(memory, fd);
    g_assert_cmpuint(alias[4096], ==, 0x37);
    alias[8191] = 0x42;
    g_assert_cmpuint(alias[8191], ==, 0x42);
    g_assert_true(UnmapViewOfFile(alias));
    g_assert_true(CloseHandle(section));
}

static void test_large_sparse_file(void)
{
    Error *error = NULL;
    int fd = -1;
    const uint64_t length = (1ULL << 32) + 65536;
    uint8_t *memory;
    LARGE_INTEGER extent;

    if (sizeof(size_t) < sizeof(length)) {
        g_test_skip("4 GiB mapping requires a 64-bit host address space");
        return;
    }
    memory = qemu_win32_shared_ram_alloc(length, &fd, &error);
    g_assert_null(error);
    g_assert_nonnull(memory);
    g_assert_true(GetFileSizeEx((HANDLE)_get_osfhandle(fd), &extent));
    g_assert_cmpuint(extent.QuadPart, ==, length);
    /* Touch only the last page: the file and VA are large, not its disk usage. */
    g_assert_cmpuint(memory[length - 1], ==, 0);
    memory[length - 1] = 0x19;
    g_assert_cmpuint(memory[length - 1], ==, 0x19);
    qemu_win32_shared_ram_free(memory, fd);
}

static void test_failed_mapping_releases_handles(void)
{
    DWORD before, after;
    int iteration;

    /* Initialize the error-reporting path before counting its process-wide
     * resources. Every subsequent failed allocation must preserve that count.
     */
    {
        Error *error = NULL;
        int fd = -1;

        g_assert_null(qemu_win32_shared_ram_alloc(0, &fd, &error));
        g_assert_nonnull(error);
        g_assert_cmpint(fd, ==, -1);
        error_free(error);
    }
    g_assert_true(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (iteration = 0; iteration < 16; iteration++) {
        Error *error = NULL;
        int fd = -1;

        /* Windows refuses a mapping over an empty file after file creation. */
        g_assert_null(qemu_win32_shared_ram_alloc(0, &fd, &error));
        g_assert_nonnull(error);
        g_assert_cmpint(fd, ==, -1);
        error_free(error);
        g_assert_true(GetProcessHandleCount(GetCurrentProcess(), &after));
        g_assert_cmpuint(after, ==, before);
    }
    g_assert_true(GetProcessHandleCount(GetCurrentProcess(), &after));
    g_assert_cmpuint(after, ==, before);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/win32/shared-ram/alias-lifetime", test_alias_lifetime);
    g_test_add_func("/win32/shared-ram/large-sparse-file", test_large_sparse_file);
    g_test_add_func("/win32/shared-ram/failure-cleanup",
                    test_failed_mapping_releases_handles);
    return g_test_run();
}
