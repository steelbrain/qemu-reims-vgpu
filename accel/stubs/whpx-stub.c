/*
 * WHPX stubs for QEMU
 *
 *  Copyright (c) Linaro
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "system/whpx.h"

bool whpx_allowed;
bool whpx_irqchip_in_kernel;

bool whpx_send_msi_direct(uint64_t address, uint32_t data)
{
    return false;
}

bool whpx_map_ahead(void *host, uint64_t gpa, uint64_t size)
{
    return false;
}

void whpx_unmap_ahead(uint64_t gpa, uint64_t size, void *host)
{
}

bool whpx_unmap_before_del(uint64_t gpa, uint64_t size)
{
    return false;
}
