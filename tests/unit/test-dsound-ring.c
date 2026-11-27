/*
 * The DirectSound audio driver's playback buffer arithmetic
 * (audio/dsound-ring.h), which audio/dsoundaudio.c builds with on Windows
 * alone, held on any host.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "audio/dsound-ring.h"

/*
 * The reserve past the write cursor is 60 ms of whole frames, as far as the
 * play cursor at most: 2646 frames at 44100 Hz in the default 185.76 ms
 * buffer of 16-bit stereo, not half the buffer.
 */
static void test_reserve(void)
{
    g_assert_cmpuint(dsound_reserve(44100, 4, 32768, 1000), ==, 2646 * 4);
    g_assert_cmpuint(dsound_reserve(48000, 4, 32768, 0), ==, 2880 * 4);
    /* A buffer too short for it: up to the play cursor, in whole frames. */
    g_assert_cmpuint(dsound_reserve(48000, 6, 6000, 1001), ==, 4998);
    g_assert_cmpuint(dsound_reserve(44100, 4, 32768, 32764), ==, 4);
}

/*
 * The cursors in whole frames: the play cursor back to its frame's start,
 * the write cursor on to the next frame's, wrapping at the buffer's end.
 */
static void test_cursor_frames(void)
{
    g_assert_cmpuint(dsound_play_frame(4099, 4), ==, 4096);
    g_assert_cmpuint(dsound_play_frame(4096, 4), ==, 4096);
    g_assert_cmpuint(dsound_write_frame(4097, 4, 32768), ==, 4100);
    g_assert_cmpuint(dsound_write_frame(4100, 4, 32768), ==, 4100);
    g_assert_cmpuint(dsound_write_frame(32767, 4, 32768), ==, 0);
    g_assert_cmpuint(dsound_write_frame(5, 6, 6000), ==, 6);
}

/*
 * What the play cursor passed leaves the rest to play; nothing where it
 * passed all of it, or where three quarters of a turn went by since.
 */
static void test_filled_after(void)
{
    const int64_t turn = 185760000;
    const int64_t ms = 1000000;

    g_assert_cmpuint(dsound_filled_after(8000, 3000, 10 * ms, turn), ==, 5000);
    g_assert_cmpuint(dsound_filled_after(8000, 8000, 10 * ms, turn), ==, 0);
    g_assert_cmpuint(dsound_filled_after(8000, 9000, 10 * ms, turn), ==, 0);
    g_assert_cmpuint(dsound_filled_after(8000, 3000, turn / 4 * 3 - 1, turn),
                     ==, 5000);
    g_assert_cmpuint(dsound_filled_after(8000, 3000, turn / 4 * 3, turn),
                     ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    /* Each computation says its own failure. */
    g_test_set_nonfatal_assertions();
    g_test_add_func("/dsound-ring/reserve", test_reserve);
    g_test_add_func("/dsound-ring/cursor_frames", test_cursor_frames);
    g_test_add_func("/dsound-ring/filled_after", test_filled_after);
    return g_test_run();
}
