/*
 * QEMU reims-graphics device: its calls into KVM
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * reims-graphics-pci.c is built once for every target (system_ss), and a
 * target without KVM links neither kvm_check_extension nor kvm_vm_ioctl. The
 * device's calls into KVM are therefore in reims-graphics-pci-kvm.c, which
 * only the targets with KVM link; the others link
 * reims-graphics-pci-kvm-stub.c (hw/display/meson.build). Linux hosts alone
 * build either.
 */

#ifndef HW_DISPLAY_REIMS_GRAPHICS_PCI_KVM_H
#define HW_DISPLAY_REIMS_GRAPHICS_PCI_KVM_H

#include "reims-graphics-pci-prefault.h"

/*
 * Turns KVM's KVM_X86_QUIRK_SLOT_ZAP_ALL off for the VM, where KVM runs it
 * and has the quirk. False only where KVM refused to turn it off.
 */
bool reims_graphics_kvm_disable_slot_zap_all(void);

/*
 * The pool's populate on KVM (reims-graphics-pci-prefault.h), under the
 * BQL at realize, once the VM's vCPUs are made: NULL where KVM has no
 * KVM_CAP_PRE_FAULT_MEMORY or the host is not x86-64.
 */
const ReimsGraphicsPrefaultOps *reims_graphics_kvm_prefault_ops(void);

#endif
