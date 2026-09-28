/*
 * os-win32.c
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2010 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include <windows.h>
#include <mmsystem.h>
#include "system/runstate.h"

static BOOL WINAPI qemu_ctrl_handler(DWORD type)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_SIGNAL);
    /* Windows 7 kills application when the function returns.
       Sleep here to give QEMU a try for closing.
       Sleep period is 10000ms because Windows kills the program
       after 10 seconds anyway. */
    Sleep(10000);

    return TRUE;
}

static TIMECAPS mm_tc;

static void os_undo_timer_resolution(void)
{
    timeEndPeriod(mm_tc.wPeriodMin);
}

/*
 * Keep the timer resolution asked for above, and full execution speed, when
 * our windows are hidden.
 *
 * Since Windows 11 the system stops honouring a process's timeBeginPeriod()
 * once none of its windows is visible (occluded, minimised) and falls back to
 * the 15.6 ms default tick, and may run the process under EcoQoS. Every
 * sub-tick wait in QEMU then rounds up to 15.6 ms: a device thread polling
 * every 4 ms runs at 64 Hz. With reims-vgpu that is the display VBL, and a
 * macOS guest whose display comes up while the QEMU window is covered latches
 * its compositor to ~60 Hz for the whole boot (measured with the window
 * covered: VBL delivered at 64.3 Hz without this opt-out, ~120 Hz with it).
 *
 * Opting out is documented by Microsoft: ProcessPowerThrottling with the
 * IGNORE_TIMER_RESOLUTION (and EXECUTION_SPEED, for EcoQoS) bits set in
 * ControlMask and clear in StateMask. It is per process and needs no
 * privilege; on Windows versions that do not know the information class the
 * call fails harmlessly.
 */
static void os_keep_timer_resolution(void)
{
    PROCESS_POWER_THROTTLING_STATE state = {
        .Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION |
                       PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
        .StateMask = 0,     /* honour timer requests; no EcoQoS */
    };

    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                          &state, sizeof(state));
}

void os_setup_early_signal_handling(void)
{
    SetConsoleCtrlHandler(qemu_ctrl_handler, TRUE);
    timeGetDevCaps(&mm_tc, sizeof(mm_tc));
    timeBeginPeriod(mm_tc.wPeriodMin);
    atexit(os_undo_timer_resolution);
    os_keep_timer_resolution();
}

void os_set_line_buffering(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
}
