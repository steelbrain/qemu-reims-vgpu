/*
 * QEMU DirectSound audio driver: its playback buffer's arithmetic
 *
 * Moved out of dsoundaudio.c, which builds only on Windows, unchanged, so
 * that tests/unit/test-dsound-ring.c holds it on any host.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AUDIO_DSOUND_RING_H
#define AUDIO_DSOUND_RING_H

#include "qemu/host-utils.h"

/*
 * The reserve past the write cursor. An x86-64 macOS guest posts its periods
 * on a 41.67 ms grid, their arrivals spread over 43 to 53 ms about their pace,
 * and DirectSound's cursors move in 10 ms steps: 60 ms covers both, where the
 * voice is written as each period comes.
 */
#define DSOUND_RESERVE_US 60000

/*
 * The reserve in bytes of a voice of @freq frames a second and @bpf bytes a
 * frame in a buffer of @size_emul bytes, where the write cursor leads the
 * play cursor by @lead bytes: whole frames, as far as the play cursor at most.
 */
static inline size_t dsound_reserve(int freq, size_t bpf, size_t size_emul,
                                    size_t lead)
{
    size_t reserve = (size_t)muldiv64(freq, DSOUND_RESERVE_US,
                                      1000000) * bpf;

    return MIN(reserve, size_emul - lead) / bpf * bpf;
}

/* The play cursor @ppos at its frame's start. */
static inline size_t dsound_play_frame(size_t ppos, size_t bpf)
{
    return ppos / bpf * bpf;
}

/* The first frame's start at or past the write cursor @wpos, in the buffer. */
static inline size_t dsound_write_frame(size_t wpos, size_t bpf,
                                        size_t size_emul)
{
    return (wpos + bpf - 1) / bpf * bpf % size_emul;
}

/*
 * The bytes from the play cursor to the write position once the play cursor
 * has moved @played bytes, @since_ns after its last read, of @filled before:
 * none where it passed them all, or where most of a turn of the buffer
 * (@buffer_ns) went by since, short of a stall of the main loop.
 */
static inline size_t dsound_filled_after(size_t filled, size_t played,
                                         int64_t since_ns, int64_t buffer_ns)
{
    if (played >= filled ||
        since_ns >= buffer_ns / 4 * 3) {
        return 0;
    }
    return filled - played;
}

#endif
