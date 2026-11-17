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
/*
 * Maps @size bytes of host memory at @host into the guest at @gpa, read,
 * write and execute, on any thread, without the BQL, ahead of the add of the
 * RAM region that holds exactly that memory there: the listener's add of that
 * section then maps nothing, so the hypervisor's map, which takes about 26 ms
 * a GiB, is not spent under the BQL. The range must hold nothing in the
 * guest's address space until that add. False, with nothing mapped, where the
 * hypervisor refused it; the add then maps it as usual.
 */
bool whpx_map_ahead(void *host, uint64_t gpa, uint64_t size);
/*
 * Undoes a whpx_map_ahead of exactly @gpa, @size and @host whose region was
 * never added; nothing where the add took it.
 */
void whpx_unmap_ahead(uint64_t gpa, uint64_t size, void *host);
/*
 * Unmaps the @size bytes at @gpa of a RAM region the device is about to take
 * out of the guest's address space, on any thread, without the BQL: the
 * listener's removal of exactly that section then unmaps nothing, so the
 * hypervisor's unmap, which takes about 45 ms a GiB, is not spent under the
 * BQL. Until the removal a guest access there exits, and QEMU carries it out
 * on the region's memory. False, with nothing unmapped, where the region's add
 * mapped ahead is still to come or the hypervisor refused; the removal then
 * unmaps it as usual.
 */
bool whpx_unmap_before_del(uint64_t gpa, uint64_t size);
#else /* !CONFIG_WHPX_IS_POSSIBLE */
#define whpx_enabled() 0
#define whpx_irqchip_in_kernel() (0)
#endif /* !CONFIG_WHPX_IS_POSSIBLE */

#endif /* QEMU_WHPX_H */
