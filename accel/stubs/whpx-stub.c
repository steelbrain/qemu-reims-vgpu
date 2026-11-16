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
