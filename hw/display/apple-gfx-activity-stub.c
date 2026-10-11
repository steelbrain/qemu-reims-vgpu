/*
 * apple-gfx's activity against App Nap, on hosts without App Nap
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Only macOS naps an application none of whose windows the person sees
 * (apple-gfx-activity.m). Elsewhere a realized device's hold does nothing.
 */

#include "qemu/osdep.h"
#include "apple-gfx.h"

void apple_gfx_activity_begin(void)
{
}

void apple_gfx_activity_end(void)
{
}
