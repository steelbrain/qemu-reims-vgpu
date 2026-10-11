/*
 * apple-gfx's activity against App Nap
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * macOS naps an application none of whose windows the person sees: QEMU
 * behind another application's windows, or hidden. App Nap lowers every
 * thread of the process to priority 4, vCPUs and the library's threads
 * among them, and throttles its timers; Metal's compiler service then runs
 * the process's shader compiles at that priority too, and a guest's frames
 * stall for seconds. While an apple-gfx device is realized, QEMU holds a
 * user-initiated activity, which keeps App Nap away. It still lets the host
 * sleep when idle, as the host's own settings say.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "apple-gfx.h"

#import <Foundation/Foundation.h>

/* The activity, and the realized devices that hold it (BQL protected). */
static id<NSObject> apple_gfx_activity;
static unsigned int apple_gfx_activity_holders;

void apple_gfx_activity_begin(void)
{
    NSActivityOptions options = NSActivityUserInitiatedAllowingIdleSystemSleep;

    assert(bql_locked());
    if (apple_gfx_activity_holders++ > 0) {
        return;
    }
    @autoreleasepool {
        NSString *reason = @"apple-gfx: a virtual machine's GPU and displays";

        apple_gfx_activity = [[[NSProcessInfo processInfo]
                                  beginActivityWithOptions:options
                                                    reason:reason] retain];
    }
}

void apple_gfx_activity_end(void)
{
    assert(bql_locked());
    assert(apple_gfx_activity_holders > 0);
    if (--apple_gfx_activity_holders > 0) {
        return;
    }
    @autoreleasepool {
        [[NSProcessInfo processInfo] endActivity:apple_gfx_activity];
    }
    [apple_gfx_activity release];
    apple_gfx_activity = nil;
}
