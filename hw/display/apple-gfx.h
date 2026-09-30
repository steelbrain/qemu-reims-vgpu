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
#include "qemu/queue.h"
#include "qemu/thread.h"
#include <reims_vgpu.h>

#define TYPE_APPLE_GFX_MMIO         "apple-gfx-mmio"
#define TYPE_APPLE_GFX_PCI          "apple-gfx-pci"

typedef struct AppleGFXDisplayMode {
    uint16_t width_px;
    uint16_t height_px;
    uint16_t refresh_rate_hz;
} AppleGFXDisplayMode;

typedef struct AppleGFXState AppleGFXState;

/* One of the device's displays: the i-th is on port i, and console head i. */
typedef struct AppleGFXDisplay {
    /* Initialised on realize() */
    AppleGFXState *state;
    RvgDisplay *pgdisp;
    QemuConsole *con;

    /* Mutable state (BQL protected) */
    QEMUCursor *cursor;
    DisplaySurface *surface;
    RvgFrameTarget *target; /* The surface's size, BGRA8; NULL if refused */
    int8_t pending_frames; /* # guest frames in the rendering pipeline */
    bool gfx_update_requested; /* QEMU display system wants a new frame */
    bool new_frame_ready; /* Guest has rendered a frame, ready to be used */
    uint32_t rendering_frame_width;
    uint32_t rendering_frame_height;
    bool cursor_show;
} AppleGFXDisplay;

/* Callbacks' work for the BQL, in the order the callbacks came. */
typedef struct AppleGFXEvent AppleGFXEvent;
typedef QSIMPLEQ_HEAD(, AppleGFXEvent) AppleGFXEventQueue;

struct AppleGFXState {
    /* Initialised on init/realize() */
    MemoryRegion iomem_gfx;
    RvgDevice *pgdev;
    /*
     * Runs the library's MMIO calls, which may block and call back, off the
     * vCPU thread, and the frame encodes off the main loop.
     */
    ThreadPool *pool;
    AppleGFXDisplayMode *display_modes;
    uint32_t num_display_modes;
    uint32_t num_displays; /* 1 to RVG_MAX_DISPLAY_PORTS, on ports 0 up */
    AppleGFXDisplay displays[RVG_MAX_DISPLAY_PORTS];

    /*
     * The display callbacks' cursor work, queued on the library's display
     * thread and run under the BQL by event_bh.
     */
    QemuMutex event_lock;
    AppleGFXEventQueue events; /* event_lock protected */
    QEMUBH *event_bh;
};

void apple_gfx_common_init(Object *obj, AppleGFXState *s, const char* obj_name);
/*
 * Makes the device from @config, which the bus variant fills in with its
 * callbacks and flags, then its displays and their consoles.
 */
bool apple_gfx_common_realize(AppleGFXState *s, DeviceState *dev,
                              RvgDeviceConfig *config, Error **errp);
/* The guest's RAM, as RvgMemoryRange elements: an RvgMemoryMap's ranges. */
GArray *apple_gfx_new_memory_ranges(void);

extern const PropertyInfo qdev_prop_apple_gfx_display_mode;

#endif
