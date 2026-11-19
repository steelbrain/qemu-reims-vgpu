/*
 * QEMU reims-graphics device: the pool's pre-fault
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A pool piece's first touch by the guest takes one second-level fault a
 * 4 KiB page: 140,289 of them, about 180-260 ms, for a 548 MiB piece on
 * KVM. Once the piece is in the guest and its add answered, one thread of
 * the VM's fills the second-level tables for it ahead of the guest's
 * touch, so the guest's first touch takes no fault: the populate. The
 * answer never waits for it.
 *
 * The thread takes the pieces lowest address first, each in 2 MiB slices,
 * one call a slice, and stops a piece between slices at its removal. Each
 * call holds what a slot change waits for (KVM's SRCU), so a slice bounds
 * that wait. The accelerator's part, the call, is a
 * ReimsGraphicsPrefaultOps: KVM's KVM_PRE_FAULT_MEMORY on a vCPU of the
 * thread's own that never runs (reims-graphics-pci-kvm.c).
 *
 * A pool piece's add or removal changes a slot under the BQL, which must
 * not wait for a call: it pauses the populate first, which signals the
 * call in flight to end it at once, and resumes it once its slot change
 * is done.
 */

#ifndef HW_DISPLAY_REIMS_GRAPHICS_PCI_PREFAULT_H
#define HW_DISPLAY_REIMS_GRAPHICS_PCI_PREFAULT_H

#include "exec/hwaddr.h"
#include "qemu/units.h"

/* The most a call populates: what a slot change waits for, at most. */
#define REIMS_GRAPHICS_PREFAULT_SLICE (2 * MiB)

typedef struct ReimsGraphicsPrefaultOps {
    /*
     * On the populate thread, once, before any call: false, with @errp
     * set, where it cannot populate.
     */
    bool (*start)(Error **errp);
    /*
     * On the populate thread: populates [@gpa, @gpa + @size), at most one
     * slice. The bytes done from @gpa (> 0), or a negative errno where
     * none were.
     */
    int64_t (*populate)(hwaddr gpa, uint64_t size);
    /*
     * Any thread: signals the populate thread, so that the call it is in
     * ends with what it has done (-EINTR where it did nothing).
     */
    void (*kick)(void);
} ReimsGraphicsPrefaultOps;

typedef struct ReimsGraphicsPrefault ReimsGraphicsPrefault;

/*
 * The VM's populate thread, under the BQL: made at the first call, which
 * waits for its start. NULL where the accelerator has no populate or its
 * start failed (a warning says why).
 */
ReimsGraphicsPrefault *reims_graphics_prefault_get(void);

/*
 * Queues [@gpa, @gpa + @size) for the populate, once the guest's address
 * space has it and its add was answered. Any thread.
 */
void reims_graphics_prefault_add(ReimsGraphicsPrefault *pf, hwaddr gpa,
                                 uint64_t size);

/*
 * Drops what is queued of [@gpa, @gpa + @size), and stops the piece in
 * flight there before its next slice. Any thread; it waits for no call.
 */
void reims_graphics_prefault_stop(ReimsGraphicsPrefault *pf, hwaddr gpa,
                                  uint64_t size);

/*
 * Under the BQL, before a slot change: no call starts until the matching
 * resume, and the call in flight is ended; returns once none is.
 */
void reims_graphics_prefault_pause(ReimsGraphicsPrefault *pf);
void reims_graphics_prefault_resume(ReimsGraphicsPrefault *pf);

/* Traces the thread's totals so far. */
void reims_graphics_prefault_report(ReimsGraphicsPrefault *pf);

#endif
