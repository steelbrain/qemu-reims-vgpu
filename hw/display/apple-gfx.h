/*
 * Data structures and functions shared between variants of the apple-gfx
 * display adapter, which drives reims-vgpu's C API (reims_vgpu.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_APPLE_GFX_H
#define QEMU_APPLE_GFX_H

#include "system/memory.h"
#include "hw/core/qdev-properties.h"
#include "ui/surface.h"
#include "block/thread-pool.h"
#include <ReimsVGPU/reims_vgpu.h>

#define TYPE_APPLE_GFX_MMIO         "apple-gfx-mmio"
#define TYPE_APPLE_GFX_PCI          "apple-gfx-pci"

typedef struct AppleGFXDisplayMode {
    uint16_t width_px;
    uint16_t height_px;
    uint16_t refresh_rate_hz;
} AppleGFXDisplayMode;

typedef struct AppleGFXState {
    /* Initialised on init/realize() */
    MemoryRegion iomem_gfx;
    RvgDevice *pgdev;
    RvgDisplay *pgdisp;
    QemuConsole *con;
    /*
     * Runs the library's MMIO calls, which may block and call back, off the
     * vCPU thread, and the frame encodes off the main loop.
     */
    ThreadPool *pool;
    AppleGFXDisplayMode *display_modes;
    uint32_t num_display_modes;

    /* Mutable state (BQL protected) */
    QEMUCursor *cursor;
    DisplaySurface *surface;
    RvgFrameTarget *target; /* The surface's size, BGRA8; NULL if refused */
    int8_t pending_frames; /* # guest frames in the rendering pipeline */
    bool gfx_update_requested; /* QEMU display system wants a new frame */
    bool new_frame_ready; /* Guest has rendered a frame, ready to be used */
    uint32_t rendering_frame_width;
    uint32_t rendering_frame_height;

    /* Mutable state (atomic) */
    bool cursor_show;
} AppleGFXState;

void apple_gfx_common_init(Object *obj, AppleGFXState *s, const char* obj_name);
/*
 * Makes the device from @config, which the bus variant fills in with its
 * callbacks and flags, then its display and console.
 */
bool apple_gfx_common_realize(AppleGFXState *s, DeviceState *dev,
                              RvgDeviceConfig *config, Error **errp);
/* The guest's RAM, as RvgMemoryRange elements: an RvgMemoryMap's ranges. */
GArray *apple_gfx_new_memory_ranges(void);

extern const PropertyInfo qdev_prop_apple_gfx_display_mode;

#endif
