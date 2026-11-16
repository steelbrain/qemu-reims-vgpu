/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX) support
 *
 * Copyright Microsoft, Corp. 2017
 *
 * Authors:
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/* header to be included in non-WHPX-specific code */

#ifndef QEMU_WHPX_H
#define QEMU_WHPX_H

#ifdef COMPILING_PER_TARGET
# ifdef CONFIG_WHPX
#  define CONFIG_WHPX_IS_POSSIBLE
# endif /* !CONFIG_WHPX */
#else
# define CONFIG_WHPX_IS_POSSIBLE
#endif /* COMPILING_PER_TARGET */

#ifdef CONFIG_WHPX_IS_POSSIBLE
extern bool whpx_allowed;
extern bool whpx_irqchip_in_kernel;
#define whpx_enabled() (whpx_allowed)
#define whpx_irqchip_in_kernel() (whpx_irqchip_in_kernel)
/*
 * Delivers the MSI of @address and @data as its write to the APIC's MSI
 * region does, on any thread, with the BQL held or not. False, with nothing
 * delivered, where the platform's APIC is not the hypervisor's
 * (whpx_irqchip_in_kernel()), the vector is 0, or the hypervisor refused the
 * request: the caller then sends it the usual way (msix_notify), which says
 * why it is lost.
 */
bool whpx_send_msi_direct(uint64_t address, uint32_t data);
#else /* !CONFIG_WHPX_IS_POSSIBLE */
#define whpx_enabled() 0
#define whpx_irqchip_in_kernel() (0)
#endif /* !CONFIG_WHPX_IS_POSSIBLE */

#endif /* QEMU_WHPX_H */
