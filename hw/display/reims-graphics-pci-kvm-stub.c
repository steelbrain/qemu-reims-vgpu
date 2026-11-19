/*
 * QEMU reims-graphics device: its calls into KVM, for the targets without KVM
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "reims-graphics-pci-kvm.h"

bool reims_graphics_kvm_disable_slot_zap_all(void)
{
    return true;
}

const ReimsGraphicsPrefaultOps *reims_graphics_kvm_prefault_ops(void)
{
    return NULL;
}
