/*
 * Reims VGPU — guest pages as a Linux dma-buf.
 *
 * The GPU reaches guest memory through a dma-buf rather than a host pointer.
 * The distinction is the whole reason this file exists: a dma-buf names an
 * explicit list of page ranges chosen here, it is revoked by closing the fd,
 * and the kernel — not this process — hands the importing driver its reference.
 * A host-pointer import has none of those three properties, which is why it is
 * banned crate-wide and why this is not that.
 *
 * Two host preconditions, both checked rather than assumed:
 *
 *  - /dev/udmabuf must exist and be openable by this process.
 *  - Guest RAM must be fd-backed. A plain `-m` allocation is an anonymous
 *    mapping with no fd, and no dma-buf can be made over it; the boot scripts
 *    pass `-object memory-backend-memfd,share=on` for exactly this reason.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "reims_vgpu_qemu_abi.h"
#include "reims-vgpu-dmabuf.h"

#ifdef CONFIG_LINUX

#include <sys/ioctl.h>
#include "system/ramblock.h"
#include "standard-headers/linux/udmabuf.h"

/*
 * One /dev/udmabuf handle for the process. Opening it is a permission check as
 * much as a resource acquisition (the node is typically root:kvm), and the
 * answer cannot change while the process runs, so it is asked once. -2 is the
 * "not yet attempted" state; -1 records a failed attempt so a host without the
 * node does not retry the open on every resource.
 */
static int reims_vgpu_udmabuf_fd = -2;

static int reims_vgpu_udmabuf_device(void)
{
    if (reims_vgpu_udmabuf_fd == -2) {
        reims_vgpu_udmabuf_fd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    }
    return reims_vgpu_udmabuf_fd;
}

int reims_vgpu_dmabuf_for_pages(const uint64_t *gpas, size_t count,
                                size_t page_size)
{
    struct udmabuf_create_list *list;
    size_t runs = 0;
    size_t i;
    int device;
    int fd;

    if (!gpas || count == 0 || page_size == 0) {
        return REIMS_VGPU_DMABUF_ERR_ARGS;
    }
    /*
     * udmabuf ranges are expressed in host pages. A guest page smaller than a
     * host page cannot be named on its own, and one that is not a whole
     * multiple would round — silently covering bytes the caller did not ask
     * for. Both are refused here rather than handed to the ioctl.
     */
    if (page_size % (size_t)qemu_real_host_page_size() != 0) {
        return REIMS_VGPU_DMABUF_ERR_PAGE_SIZE;
    }
    device = reims_vgpu_udmabuf_device();
    if (device < 0) {
        return REIMS_VGPU_DMABUF_ERR_UNSUPPORTED;
    }

    list = g_malloc0(sizeof(*list) +
                     sizeof(struct udmabuf_create_item) *
                         REIMS_VGPU_DMABUF_MAX_RUNS);

    rcu_read_lock();
    for (i = 0; i < count; i++) {
        hwaddr xlat, plen = page_size;
        MemoryRegion *mr;
        RAMBlock *rb;
        ram_addr_t offset;
        uint8_t *hva;
        uint64_t fd_offset;

        if ((gpas[i] & (page_size - 1)) != 0) {
            rcu_read_unlock();
            g_free(list);
            return REIMS_VGPU_DMABUF_ERR_ALIGNMENT;
        }
        mr = address_space_translate(&address_space_memory, gpas[i], &xlat,
                                     &plen, true, MEMTXATTRS_UNSPECIFIED);
        if (!mr || !memory_region_is_ram(mr) || plen < page_size) {
            rcu_read_unlock();
            g_free(list);
            return REIMS_VGPU_DMABUF_ERR_NOT_RAM;
        }
        hva = (uint8_t *)memory_region_get_ram_ptr(mr) + xlat;
        rb = qemu_ram_block_from_host(hva, false, &offset);
        if (!rb || qemu_ram_get_fd(rb) < 0) {
            rcu_read_unlock();
            g_free(list);
            return REIMS_VGPU_DMABUF_ERR_NOT_MEMFD;
        }
        /*
         * A RAMBlock may sit at a non-zero offset inside its backing file, so
         * the byte at block offset `offset` is at `fd_offset + offset` in the
         * fd. Dropping the block's own offset would build a dma-buf over the
         * wrong bytes of the right file — which reads as corruption, not as a
         * failure.
         */
        fd_offset = (uint64_t)qemu_ram_get_fd_offset(rb) + (uint64_t)offset;

        /*
         * Coalesce physically adjacent pages. A guest surface is usually
         * backed by long runs, and one item per page would hit the kernel's
         * list bound at a few megabytes while one item per run covers a whole
         * framebuffer.
         */
        if (runs > 0 &&
            list->list[runs - 1].memfd == (uint64_t)qemu_ram_get_fd(rb) &&
            list->list[runs - 1].offset + list->list[runs - 1].size ==
                fd_offset) {
            list->list[runs - 1].size += page_size;
            continue;
        }
        if (runs == REIMS_VGPU_DMABUF_MAX_RUNS) {
            rcu_read_unlock();
            g_free(list);
            return REIMS_VGPU_DMABUF_ERR_TOO_FRAGMENTED;
        }
        list->list[runs].memfd = (uint64_t)qemu_ram_get_fd(rb);
        list->list[runs].offset = fd_offset;
        list->list[runs].size = page_size;
        runs++;
    }
    rcu_read_unlock();

    list->count = runs;
    list->flags = UDMABUF_FLAGS_CLOEXEC;
    fd = ioctl(device, UDMABUF_CREATE_LIST, list);
    g_free(list);
    if (fd < 0) {
        return REIMS_VGPU_DMABUF_ERR_CREATE;
    }
    return fd;
}

#else /* !CONFIG_LINUX */

/*
 * dma-buf is a Linux kernel object. On any other host the answer is the same
 * every time and is given here rather than left to a missing symbol, so the
 * Rust side sees one named refusal instead of a link error.
 */
int reims_vgpu_dmabuf_for_pages(const uint64_t *gpas, size_t count,
                                size_t page_size)
{
    (void)gpas;
    (void)count;
    (void)page_size;
    return REIMS_VGPU_DMABUF_ERR_UNSUPPORTED;
}

#endif /* CONFIG_LINUX */
