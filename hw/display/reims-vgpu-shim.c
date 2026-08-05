/*
 * reims-vgpu: bus-independent shim services. See reims-vgpu-shim.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "exec/cpu-common.h"
#include "system/address-spaces.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "ui/console.h"
#include "ui/input.h"
#include "reims_vgpu_qemu_abi.h"
#include "reims-vgpu-shim.h"

uint64_t reims_vgpu_shim_mono_ns(void *ctx)
{
    (void)ctx;
    return (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_HOST);
}

/*
 * RAM-only attrs. `memory = 1` makes the address space reject a translation
 * that resolves to a device rather than RAM (MEMTX_ACCESS_ERROR) instead of
 * performing it. Every GPA reaching read/write_gpa came from a guest-supplied
 * page-entry list, so an entry pointing at one of our own BARs would otherwise
 * re-enter this device's MMIO handler from inside a Rust call that already
 * holds the device lock. Failing closed turns that into a visible decline.
 */
static const MemTxAttrs reims_vgpu_shim_ram_attrs = {
    .memory = true,
};

int reims_vgpu_shim_read_gpa(void *ctx, uint64_t gpa, uint8_t *buf, size_t len)
{
    MemTxResult r;

    (void)ctx;
    if (!buf || len == 0) {
        return 0;
    }
    r = address_space_read(&address_space_memory, gpa,
                           reims_vgpu_shim_ram_attrs, buf, len);
    return r == MEMTX_OK ? 0 : -1;
}

int reims_vgpu_shim_write_gpa(void *ctx, uint64_t gpa, const uint8_t *buf,
                              size_t len)
{
    MemTxResult r;

    (void)ctx;
    if (!buf || len == 0) {
        return 0;
    }
    r = address_space_write(&address_space_memory, gpa,
                            reims_vgpu_shim_ram_attrs, buf, len);
    return r == MEMTX_OK ? 0 : -1;
}

int reims_vgpu_shim_is_ram_gpa(void *ctx, uint64_t gpa)
{
    hwaddr xlat, plen = 1;
    MemoryRegion *mr;
    int ok;

    (void)ctx;
    rcu_read_lock();
    mr = address_space_translate(&address_space_memory, gpa, &xlat, &plen, true,
                                 MEMTXATTRS_UNSPECIFIED);
    ok = mr && memory_region_is_ram(mr) && plen >= 1;
    rcu_read_unlock();
    return ok ? 1 : 0;
}

int reims_vgpu_shim_read_kva(void *ctx, uint64_t kva, uint8_t *buf, size_t len)
{
    CPUState *cs = current_cpu;

    (void)ctx;
    if (!buf || len == 0) {
        return 0;
    }
    if (!cs) {
        return -2;
    }
    cpu_synchronize_state(cs);
    return cpu_memory_rw_debug(cs, kva, buf, len, false) == 0 ? 0 : -1;
}

static InputButton reims_vgpu_shim_button(uint32_t code, bool *ok)
{
    *ok = true;
    switch (code) {
    case REIMS_VGPU_BUTTON_LEFT:
        return INPUT_BUTTON_LEFT;
    case REIMS_VGPU_BUTTON_MIDDLE:
        return INPUT_BUTTON_MIDDLE;
    case REIMS_VGPU_BUTTON_RIGHT:
        return INPUT_BUTTON_RIGHT;
    case REIMS_VGPU_BUTTON_WHEEL_UP:
        return INPUT_BUTTON_WHEEL_UP;
    case REIMS_VGPU_BUTTON_WHEEL_DOWN:
        return INPUT_BUTTON_WHEEL_DOWN;
    case REIMS_VGPU_BUTTON_SIDE:
        return INPUT_BUTTON_SIDE;
    case REIMS_VGPU_BUTTON_EXTRA:
        return INPUT_BUTTON_EXTRA;
    case REIMS_VGPU_BUTTON_WHEEL_LEFT:
        return INPUT_BUTTON_WHEEL_LEFT;
    case REIMS_VGPU_BUTTON_WHEEL_RIGHT:
        return INPUT_BUTTON_WHEEL_RIGHT;
    default:
        *ok = false;
        return INPUT_BUTTON_LEFT;
    }
}

bool reims_vgpu_shim_scanout_may_paint(uint64_t rust_handle, uint32_t mapping_id)
{
    uint32_t may = 0;

    if (rust_handle == 0) {
        return false;
    }
    if (reims_vgpu_qemu_scanout_may_paint(rust_handle, mapping_id, &may) !=
        REIMS_VGPU_QEMU_OK) {
        return false;
    }
    return may != 0;
}

uint32_t reims_vgpu_shim_console_feed(uint64_t rust_handle, uint32_t *out_mid,
                                      uint32_t *out_w, uint32_t *out_h,
                                      uint32_t *out_gen)
{
    uint32_t kind = REIMS_VGPU_CONSOLE_FEED_FIRMWARE;

    if (rust_handle == 0) {
        return REIMS_VGPU_CONSOLE_FEED_FIRMWARE;
    }
    if (reims_vgpu_qemu_console_feed(rust_handle, &kind, out_mid, out_w, out_h,
                                     out_gen) != REIMS_VGPU_QEMU_OK) {
        return REIMS_VGPU_CONSOLE_FEED_FIRMWARE;
    }
    return kind;
}

void reims_vgpu_shim_input_key(QemuConsole *con, uint32_t evdev, bool down)
{
    if (!con) {
        return;
    }
    qemu_input_event_send_key_linux(con, evdev, down);
}

void reims_vgpu_shim_input_pointer_move(QemuConsole *con, uint32_t x,
                                        uint32_t y, uint32_t w, uint32_t h)
{
    if (!con || w == 0 || h == 0) {
        return;
    }
    qemu_input_queue_abs(con, INPUT_AXIS_X, (int)x, 0, (int)w);
    qemu_input_queue_abs(con, INPUT_AXIS_Y, (int)y, 0, (int)h);
    qemu_input_event_sync();
}

void reims_vgpu_shim_input_button(QemuConsole *con, uint32_t code, bool down)
{
    InputButton button;
    bool ok;

    if (!con) {
        return;
    }
    button = reims_vgpu_shim_button(code, &ok);
    if (!ok) {
        return;
    }
    qemu_input_queue_btn(con, button, down);
    qemu_input_event_sync();
}
