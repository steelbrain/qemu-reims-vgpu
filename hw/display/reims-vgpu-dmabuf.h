/*
 * Reims VGPU — guest pages as a Linux dma-buf.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef REIMS_VGPU_DMABUF_H
#define REIMS_VGPU_DMABUF_H

#include <stddef.h>
#include <stdint.h>

/*
 * Export `count` page-aligned guest physical addresses, each `page_size` bytes,
 * as ONE Linux dma-buf. Returns an owned fd (>= 0) the caller must close, or one
 * of the negative REIMS_VGPU_DMABUF_ERR_* codes from the ABI header.
 *
 * The shim answers *which check refused*, never the inputs a caller would need
 * to reconstruct the rule: whether this host has udmabuf, whether guest RAM is
 * fd-backed, and whether the run list fits the kernel's bound are all questions
 * only QEMU can answer, and all of them are answered here rather than exported
 * as facts for Rust to combine.
 */
int reims_vgpu_dmabuf_for_pages(const uint64_t *gpas, size_t count,
                                size_t page_size);

#endif /* REIMS_VGPU_DMABUF_H */
