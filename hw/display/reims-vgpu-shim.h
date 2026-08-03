/*
 * reims-vgpu: shim services that do not depend on which bus attached the
 * device, shared by reims-vgpu-pci and reims-vgpu-mmio.
 *
 * A service belongs here when its body reads nothing from the device state:
 * either it ignores the HostOps `ctx` entirely (host clock, address-space
 * queries, vCPU-relative reads) or it needs only the console, which the
 * caller passes explicitly. Anything that touches the bus object, the
 * per-device dirty tracker, or a bus-specific trace event stays in its shim.
 *
 * The point is not line count. A duplicated button table or address-space
 * predicate is a table that can drift, and a drift between the two shims is
 * a bug the guest sees on exactly one pathway.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef REIMS_VGPU_SHIM_H
#define REIMS_VGPU_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "qemu/typedefs.h"

/*
 * The three below match HostOps function-pointer signatures, so both shims'
 * host_ops tables point at them directly; `ctx` is accepted and ignored.
 */

/* Host monotonic clock in nanoseconds. */
uint64_t reims_vgpu_shim_mono_ns(void *ctx);

/* 1 = guest RAM, 0 = not (MMIO / ROM / unmapped). Mapper page-entry accept. */
int reims_vgpu_shim_is_ram_gpa(void *ctx, uint64_t gpa);

/*
 * Guest kernel VA -> host buffer, for MappingInternal / page-table walks.
 *
 * MUST run on a vCPU thread with current_cpu set (typically the iosfc
 * producer MMIO path). Never falls back to first_cpu: from the drain BH
 * that would do_run_on_cpu while the vCPU may be blocked in MMIO waiting
 * for the Rust DEVICES mutex — classic AB-BA hang (UI "not responding").
 * Returns 0 on success, -2 with no current vCPU, -1 on a failed read.
 */
int reims_vgpu_shim_read_kva(void *ctx, uint64_t kva, uint8_t *buf, size_t len);

/*
 * Host-owned-window input: replay a neutral Rust input action through the
 * QEMU input subsystem. Input routes to the guest's active handlers
 * (usb-kbd / usb-tablet) independent of any display, so these work with
 * -display none. All business logic (platform key -> evdev, scroll delta ->
 * wheel notches, coordinate origin) lives in Rust; the shim only translates
 * the neutral wire form into the QEMU input ABI, which owns the QEMU-side
 * keycode and button enums. A NULL console drops the event.
 */

/* `evdev` is a Linux evdev code; QEMU drops codes it cannot map to a qcode,
 * so no shim-side keycode table is needed. */
void reims_vgpu_shim_input_key(QemuConsole *con, uint32_t evdev, bool down);

/* Absolute pointer (usb-tablet): scales window pixels into the abs range. */
void reims_vgpu_shim_input_pointer_move(QemuConsole *con, uint32_t x,
                                        uint32_t y, uint32_t w, uint32_t h);

/* `code` is a REIMS_VGPU_BUTTON_*; an unrecognised one drops the event. */
void reims_vgpu_shim_input_button(QemuConsole *con, uint32_t code, bool down);

#endif /* REIMS_VGPU_SHIM_H */
