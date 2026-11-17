/*
 * QEMU reims-graphics device: its calls into KVM, for the targets with KVM
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "system/kvm.h"
#include <linux/kvm.h>
#include "reims-graphics-pci-kvm.h"

bool reims_graphics_kvm_disable_slot_zap_all(void)
{
#ifdef KVM_X86_QUIRK_SLOT_ZAP_ALL
    struct kvm_enable_cap cap = {
        .cap = KVM_CAP_DISABLE_QUIRKS2,
        .args = { KVM_X86_QUIRK_SLOT_ZAP_ALL },
    };

    if (!kvm_enabled() ||
        !(kvm_check_extension(kvm_state, KVM_CAP_DISABLE_QUIRKS2) &
          KVM_X86_QUIRK_SLOT_ZAP_ALL)) {
        return true;
    }
    return kvm_vm_ioctl(kvm_state, KVM_ENABLE_CAP, &cap) >= 0;
#else
    return true;
#endif
}
