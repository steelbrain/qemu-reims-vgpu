/*
 * QEMU DirectSound audio driver
 *
 * Copyright (c) 2005 Vassili Karpov (malc)
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

/*
 * SEAL 1.07 by Carlos 'pel' Hasan was used as documentation
 */

#include "qemu/osdep.h"
#include "qemu/audio.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "audio_int.h"
#include "dsound-ring.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qom/object.h"

#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <dsound.h>

#include "trace.h"
#include "audio_win_int.h"

#define TYPE_AUDIO_DSOUND "audio-dsound"
OBJECT_DECLARE_SIMPLE_TYPE(AudioDsound, AUDIO_DSOUND)

static AudioBackendClass *audio_dsound_parent_class;

struct AudioDsound {
    AudioMixengBackend parent_obj;

    LPDIRECTSOUND dsound;
    LPDIRECTSOUNDCAPTURE dsound_capture;
    struct audsettings settings;
};

/*
 * A playback voice writes only past DirectSound's write cursor: the bytes
 * between its play and write cursors DirectSound has mixed already, so what is
 * written there is not heard, and the buffer's audio from one turn earlier
 * plays in its place. The write position (hw.pos_emul) is placed
 * DSOUND_RESERVE_US past the write cursor, with silence before it, when the
 * voice starts, and again when the audio written runs out before the write
 * cursor: a guest that hands its audio over in periods, their arrivals spread
 * over some 50 ms about their pace, is played from that reserve without a
 * gap, where its frontend has the voice written as each period comes
 * (virtio-snd does). The reserve is latency. What the play cursor has passed
 * becomes silence, so a gap plays silence.
 */
typedef struct {
    HWVoiceOut hw;
    LPDIRECTSOUNDBUFFER dsound_buffer;
    bool first_time;
    /* The bytes from the play cursor, at play_pos, to the write position. */
    size_t filled;
    DWORD play_pos;
    /* When play_pos was read, and the buffer's length, in nanoseconds. */
    int64_t play_ns;
    int64_t buffer_ns;
    /* Whether audio was written since the write position was placed. */
    bool primed;
    /* The times the audio written ran out before the write cursor. */
    uint32_t gaps;
} DSoundVoiceOut;

typedef struct {
    HWVoiceIn hw;
    LPDIRECTSOUNDCAPTUREBUFFER dsound_capture_buffer;
    bool first_time;
} DSoundVoiceIn;

static const char *dserror(HRESULT hr)
{
    switch (hr) {
    case DS_OK:
        return "The method succeeded";
#ifdef DS_NO_VIRTUALIZATION
    case DS_NO_VIRTUALIZATION:
        return "The buffer was created, but another 3D algorithm was substituted";
#endif
#ifdef DS_INCOMPLETE
    case DS_INCOMPLETE:
        return "The method succeeded, but not all the optional effects were obtained";
#endif
#ifdef DSERR_ACCESSDENIED
    case DSERR_ACCESSDENIED:
        return "The request failed because access was denied";
#endif
#ifdef DSERR_ALLOCATED
    case DSERR_ALLOCATED:
        return "The request failed because resources, "
               "such as a priority level, were already in use "
               "by another caller";
#endif
#ifdef DSERR_ALREADYINITIALIZED
    case DSERR_ALREADYINITIALIZED:
        return "The object is already initialized";
#endif
#ifdef DSERR_BADFORMAT
    case DSERR_BADFORMAT:
        return "The specified wave format is not supported";
#endif
#ifdef DSERR_BADSENDBUFFERGUID
    case DSERR_BADSENDBUFFERGUID:
        return "The GUID specified in an audiopath file "
               "does not match a valid mix-in buffer";
#endif
#ifdef DSERR_BUFFERLOST
    case DSERR_BUFFERLOST:
        return "The buffer memory has been lost and must be restored";
#endif
#ifdef DSERR_BUFFERTOOSMALL
    case DSERR_BUFFERTOOSMALL:
        return "The buffer size is not great enough to "
               "enable effects processing";
#endif
#ifdef DSERR_CONTROLUNAVAIL
    case DSERR_CONTROLUNAVAIL:
        return "The buffer control (volume, pan, and so on) "
               "requested by the caller is not available. "
               "Controls must be specified when the buffer is created, "
               "using the dwFlags member of DSBUFFERDESC";
#endif
#ifdef DSERR_DS8_REQUIRED
    case DSERR_DS8_REQUIRED:
        return "A DirectSound object of class CLSID_DirectSound8 or later "
               "is required for the requested functionality. "
               "For more information, see IDirectSound8 Interface";
#endif
#ifdef DSERR_FXUNAVAILABLE
    case DSERR_FXUNAVAILABLE:
        return "The effects requested could not be found on the system, "
               "or they are in the wrong order or in the wrong location; "
               "for example, an effect expected in hardware "
               "was found in software";
#endif
#ifdef DSERR_GENERIC
    case DSERR_GENERIC:
        return "An undetermined error occurred inside the DirectSound subsystem";
#endif
#ifdef DSERR_INVALIDCALL
    case DSERR_INVALIDCALL:
        return "This function is not valid for the current state of this object";
#endif
#ifdef DSERR_INVALIDPARAM
    case DSERR_INVALIDPARAM:
        return "An invalid parameter was passed to the returning function";
#endif
#ifdef DSERR_NOAGGREGATION
    case DSERR_NOAGGREGATION:
        return "The object does not support aggregation";
#endif
#ifdef DSERR_NODRIVER
    case DSERR_NODRIVER:
        return "No sound driver is available for use, "
               "or the given GUID is not a valid DirectSound device ID";
#endif
#ifdef DSERR_NOINTERFACE
    case DSERR_NOINTERFACE:
        return "The requested COM interface is not available";
#endif
#ifdef DSERR_OBJECTNOTFOUND
    case DSERR_OBJECTNOTFOUND:
        return "The requested object was not found";
#endif
#ifdef DSERR_OTHERAPPHASPRIO
    case DSERR_OTHERAPPHASPRIO:
        return "Another application has a higher priority level, "
              "preventing this call from succeeding";
#endif
#ifdef DSERR_OUTOFMEMORY
    case DSERR_OUTOFMEMORY:
        return "The DirectSound subsystem could not allocate "
               "sufficient memory to complete the caller's request";
#endif
#ifdef DSERR_PRIOLEVELNEEDED
    case DSERR_PRIOLEVELNEEDED:
        return "A cooperative level of DSSCL_PRIORITY or higher is required";
#endif
#ifdef DSERR_SENDLOOP
    case DSERR_SENDLOOP:
        return "A circular loop of send effects was detected";
#endif
#ifdef DSERR_UNINITIALIZED
    case DSERR_UNINITIALIZED:
        return "The Initialize method has not been called "
               "or has not been called successfully "
               "before other methods were called";
#endif
#ifdef DSERR_UNSUPPORTED
    case DSERR_UNSUPPORTED:
        return "The function called is not supported at this time";
#endif
    default:
        return NULL;
    }

}

static void dserror_set(Error **errp, HRESULT hr, const char *msg)
{
    const char *str = dserror(hr);

    if (str) {
        error_setg(errp, "%s: %s", msg, str);
    } else {
        error_setg(errp, "%s: Unknown (HRESULT: 0x%lx)", msg, hr);
    }
}

static void dsound_log_hresult(HRESULT hr)
{
    const char *str = dserror(hr);

    if (str) {
        error_printf(" Reason: %s", str);
    } else {
        error_printf(" Reason: Unknown (HRESULT: 0x%lx)", hr);
    }
}

static void G_GNUC_PRINTF(2, 3) dsound_logerr(HRESULT hr, const char *fmt, ...)
{
    va_list ap;

    error_printf("dsound: ");
    va_start(ap, fmt);
    error_vprintf(fmt, ap);
    va_end(ap);
    dsound_log_hresult(hr);
    error_printf("\n");
}

static void G_GNUC_PRINTF(3, 4) dsound_logerr2(HRESULT hr, const char *typ,
                                               const char *fmt, ...)
{
    va_list ap;

    error_printf("dsound: Could not initialize %s: ", typ);
    va_start(ap, fmt);
    error_vprintf(fmt, ap);
    va_end(ap);
    dsound_log_hresult(hr);
    error_printf("\n");
}


static int dsound_restore_out (LPDIRECTSOUNDBUFFER dsb, AudioDsound *s)
{
    HRESULT hr;

    hr = IDirectSoundBuffer_Restore (dsb);

    if (hr != DS_OK) {
        dsound_logerr(hr, "Could not restore playback buffer");
        return -1;
    }
    return 0;
}

#include "dsound_template.h"
#define DSBTYPE_IN
#include "dsound_template.h"
#undef DSBTYPE_IN

static int dsound_get_status_out (LPDIRECTSOUNDBUFFER dsb, DWORD *statusp,
                                  AudioDsound *s)
{
    HRESULT hr;

    hr = IDirectSoundBuffer_GetStatus (dsb, statusp);
    if (FAILED (hr)) {
        dsound_logerr(hr, "Could not get playback buffer status");
        return -1;
    }

    if (*statusp & DSBSTATUS_BUFFERLOST) {
        dsound_restore_out(dsb, s);
        return -1;
    }

    return 0;
}

static int dsound_get_status_in (LPDIRECTSOUNDCAPTUREBUFFER dscb,
                                 DWORD *statusp)
{
    HRESULT hr;

    hr = IDirectSoundCaptureBuffer_GetStatus (dscb, statusp);
    if (FAILED (hr)) {
        dsound_logerr(hr, "Could not get capture buffer status");
        return -1;
    }

    return 0;
}

static void dsound_clear_sample (HWVoiceOut *hw, LPDIRECTSOUNDBUFFER dsb,
                                 AudioDsound *s)
{
    int err;
    LPVOID p1, p2;
    DWORD blen1, blen2, len1, len2;

    err = dsound_lock_out (
        dsb,
        &hw->info,
        0,
        hw->size_emul,
        &p1, &p2,
        &blen1, &blen2,
        1,
        s
        );
    if (err) {
        return;
    }

    len1 = blen1 / hw->info.bytes_per_frame;
    len2 = blen2 / hw->info.bytes_per_frame;

    trace_dsound_clear_sample(p1, blen1, len1, p2, blen2, len2);

    if (p1 && len1) {
        audio_pcm_info_clear_buf (&hw->info, p1, len1);
    }

    if (p2 && len2) {
        audio_pcm_info_clear_buf (&hw->info, p2, len2);
    }

    dsound_unlock_out (dsb, p1, p2, blen1, blen2);
}

static void dsound_enable_out(HWVoiceOut *hw, bool enable)
{
    HRESULT hr;
    DWORD status;
    AudioDsound *s = AUDIO_DSOUND(hw->s);
    DSoundVoiceOut *ds = (DSoundVoiceOut *) hw;
    LPDIRECTSOUNDBUFFER dsb = ds->dsound_buffer;

    if (!dsb) {
        error_report("dsound: Attempt to control voice without a buffer");
        return;
    }

    if (enable) {
        if (dsound_get_status_out(dsb, &status, s)) {
            return;
        }

        if (status & DSBSTATUS_PLAYING) {
            warn_report("dsound: Voice is already playing");
            return;
        }

        dsound_clear_sample(hw, dsb, s);
        /* The next read of the cursors places the write position. */
        ds->first_time = true;

        hr = IDirectSoundBuffer_Play(dsb, 0, 0, DSBPLAY_LOOPING);
        if (FAILED(hr)) {
            dsound_logerr(hr, "Could not start playing buffer");
            return;
        }
    } else {
        if (dsound_get_status_out(dsb, &status, s)) {
            return;
        }

        if (status & DSBSTATUS_PLAYING) {
            hr = IDirectSoundBuffer_Stop(dsb);
            if (FAILED(hr)) {
                dsound_logerr(hr, "Could not stop playing buffer");
                return;
            }
        } else {
            warn_report("dsound: Voice is not playing");
        }
    }
}

/* Fill @len bytes of the buffer from @pos with silence. */
static void dsound_clear_out(HWVoiceOut *hw, DWORD pos, size_t len)
{
    DSoundVoiceOut *ds = (DSoundVoiceOut *)hw;
    size_t bpf = hw->info.bytes_per_frame;
    LPVOID p1, p2;
    DWORD blen1, blen2;

    if (!len ||
        dsound_lock_out(ds->dsound_buffer, &hw->info, pos, len, &p1, &p2,
                        &blen1, &blen2, 0, AUDIO_DSOUND(hw->s))) {
        return;
    }
    if (p1 && blen1) {
        audio_pcm_info_clear_buf(&hw->info, p1, blen1 / bpf);
    }
    if (p2 && blen2) {
        audio_pcm_info_clear_buf(&hw->info, p2, blen2 / bpf);
    }
    dsound_unlock_out(ds->dsound_buffer, p1, p2, blen1, blen2);
}

/*
 * Place the write position DSOUND_RESERVE_US past the write cursor @wpos, with
 * silence from the write cursor to it. Where the buffer is too short for that,
 * the write position goes as far as the play cursor.
 */
static void dsound_place_out(HWVoiceOut *hw, DWORD ppos, DWORD wpos)
{
    DSoundVoiceOut *ds = (DSoundVoiceOut *)hw;
    size_t bpf = hw->info.bytes_per_frame;
    size_t lead = audio_ring_dist(wpos, ppos, hw->size_emul);
    size_t reserve = dsound_reserve(hw->info.freq, bpf, hw->size_emul, lead);

    if (ds->primed) {
        ds->gaps++;
    }
    trace_dsound_place_out(hw, ppos, wpos, ds->filled, reserve, ds->primed,
                           ds->gaps);
    dsound_clear_out(hw, wpos, reserve);
    hw->pos_emul = (wpos + reserve) % hw->size_emul;
    ds->filled = lead + reserve;
    ds->primed = false;
}

static size_t dsound_buffer_get_free(HWVoiceOut *hw)
{
    DSoundVoiceOut *ds = (DSoundVoiceOut *) hw;
    LPDIRECTSOUNDBUFFER dsb = ds->dsound_buffer;
    size_t bpf = hw->info.bytes_per_frame;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    size_t played;
    HRESULT hr;
    DWORD ppos, wpos;

    hr = IDirectSoundBuffer_GetCurrentPosition(dsb, &ppos, &wpos);
    if (FAILED(hr)) {
        dsound_logerr(hr, "Could not get playback buffer position");
        return 0;
    }
    /* Whole frames: the play cursor's frame, and the first past the write's. */
    ppos = dsound_play_frame(ppos, bpf);
    wpos = dsound_write_frame(wpos, bpf, hw->size_emul);

    /*
     * The play cursor's advance since the last read, which reads it more
     * often than once a turn of the buffer, short of a stall of the main
     * loop: after one of most of a turn, the audio written counts as played.
     * What it played becomes silence, which a gap then plays rather than the
     * audio of a turn earlier.
     */
    played = audio_ring_dist(ppos, ds->play_pos, hw->size_emul);
    if (ds->first_time) {
        ds->first_time = false;
        ds->primed = false;
    } else {
        dsound_clear_out(hw, ds->play_pos, played);
        ds->filled = dsound_filled_after(ds->filled, played,
                                         now - ds->play_ns, ds->buffer_ns);
    }
    ds->play_pos = ppos;
    ds->play_ns = now;

    /*
     * Until audio comes, the write position keeps its place past the write
     * cursor; once the audio written runs out before the write cursor, it
     * takes it again, a gap of silence.
     */
    if (!ds->primed ||
        ds->filled < audio_ring_dist(wpos, ppos, hw->size_emul)) {
        dsound_place_out(hw, ppos, wpos);
    }
    trace_dsound_buffer_get_free(hw, ppos, wpos, ds->filled);

    return hw->size_emul - ds->filled;
}

static void *dsound_get_buffer_out(HWVoiceOut *hw, size_t *size)
{
    DSoundVoiceOut *ds = (DSoundVoiceOut *)hw;
    LPDIRECTSOUNDBUFFER dsb = ds->dsound_buffer;
    DWORD act_size;
    size_t req_size;
    int err;
    void *ret;

    /* Never over audio not yet played. */
    req_size = MIN(*size, hw->size_emul - hw->pos_emul);
    req_size = MIN(req_size, hw->size_emul - ds->filled);
    if (!req_size) {
        *size = 0;
        return NULL;
    }

    err = dsound_lock_out(dsb, &hw->info, hw->pos_emul, req_size, &ret, NULL,
                          &act_size, NULL, false, AUDIO_DSOUND(hw->s));
    if (err) {
        error_report("dsound: Failed to lock buffer");
        *size = 0;
        return NULL;
    }

    *size = act_size;
    return ret;
}

static size_t dsound_put_buffer_out(HWVoiceOut *hw, void *buf, size_t len)
{
    DSoundVoiceOut *ds = (DSoundVoiceOut *) hw;
    LPDIRECTSOUNDBUFFER dsb = ds->dsound_buffer;
    int err = dsound_unlock_out(dsb, buf, NULL, len, 0);

    if (err) {
        error_report("dsound: Failed to unlock buffer");
        return 0;
    }
    hw->pos_emul = (hw->pos_emul + len) % hw->size_emul;
    ds->filled += len;
    if (len) {
        ds->primed = true;
    }

    return len;
}

static void dsound_enable_in(HWVoiceIn *hw, bool enable)
{
    HRESULT hr;
    DWORD status;
    DSoundVoiceIn *ds = (DSoundVoiceIn *) hw;
    LPDIRECTSOUNDCAPTUREBUFFER dscb = ds->dsound_capture_buffer;

    if (!dscb) {
        error_report("dsound: Attempt to control capture voice without a buffer");
        return;
    }

    if (enable) {
        if (dsound_get_status_in(dscb, &status)) {
            return;
        }

        if (status & DSCBSTATUS_CAPTURING) {
            warn_report("dsound: Voice is already capturing");
            return;
        }

        /* clear ?? */

        hr = IDirectSoundCaptureBuffer_Start(dscb, DSCBSTART_LOOPING);
        if (FAILED(hr)) {
            dsound_logerr(hr, "Could not start capturing");
            return;
        }
    } else {
        if (dsound_get_status_in(dscb, &status)) {
            return;
        }

        if (status & DSCBSTATUS_CAPTURING) {
            hr = IDirectSoundCaptureBuffer_Stop(dscb);
            if (FAILED(hr)) {
                dsound_logerr(hr, "Could not stop capturing");
                return;
            }
        } else {
            warn_report("dsound: Voice is not capturing");
        }
    }
}

static void *dsound_get_buffer_in(HWVoiceIn *hw, size_t *size)
{
    DSoundVoiceIn *ds = (DSoundVoiceIn *) hw;
    LPDIRECTSOUNDCAPTUREBUFFER dscb = ds->dsound_capture_buffer;
    HRESULT hr;
    DWORD rpos, act_size;
    size_t req_size;
    int err;
    void *ret;

    hr = IDirectSoundCaptureBuffer_GetCurrentPosition(dscb, NULL, &rpos);
    if (FAILED(hr)) {
        dsound_logerr(hr, "Could not get capture buffer position");
        *size = 0;
        return NULL;
    }

    if (ds->first_time) {
        hw->pos_emul = rpos;
        ds->first_time = false;
    }

    req_size = audio_ring_dist(rpos, hw->pos_emul, hw->size_emul);
    req_size = MIN(*size, MIN(req_size, hw->size_emul - hw->pos_emul));

    if (req_size == 0) {
        *size = 0;
        return NULL;
    }

    err = dsound_lock_in(dscb, &hw->info, hw->pos_emul, req_size, &ret, NULL,
                         &act_size, NULL, false, AUDIO_DSOUND(hw->s));
    if (err) {
        error_report("dsound: Failed to lock buffer");
        *size = 0;
        return NULL;
    }

    *size = act_size;
    return ret;
}

static void dsound_put_buffer_in(HWVoiceIn *hw, void *buf, size_t len)
{
    DSoundVoiceIn *ds = (DSoundVoiceIn *) hw;
    LPDIRECTSOUNDCAPTUREBUFFER dscb = ds->dsound_capture_buffer;
    int err = dsound_unlock_in(dscb, buf, NULL, len, 0);

    if (err) {
        error_report("dsound: Failed to unlock buffer");
        return;
    }
    hw->pos_emul = (hw->pos_emul + len) % hw->size_emul;
}

static void audio_dsound_finalize(Object *obj)
{
    AudioDsound *s = AUDIO_DSOUND(obj);
    HRESULT hr;

    if (!s->dsound) {
        return;
    }

    hr = IDirectSound_Release (s->dsound);
    if (FAILED (hr)) {
        dsound_logerr(hr, "Could not release DirectSound");
    }
    s->dsound = NULL;

    if (!s->dsound_capture) {
        return;
    }

    hr = IDirectSoundCapture_Release (s->dsound_capture);
    if (FAILED (hr)) {
        dsound_logerr(hr, "Could not release DirectSoundCapture");
    }
    s->dsound_capture = NULL;
}

static bool
audio_dsound_realize(AudioBackend *abe, Audiodev *dev, Error **errp)
{
    AudioDsound *s = AUDIO_DSOUND(abe);
    HRESULT hr;
    AudiodevDsoundOptions *dso;

    assert(dev->driver == AUDIODEV_DRIVER_DSOUND);

    if (!audio_dsound_parent_class->realize(abe, dev, errp)) {
        return false;
    }

    dso = &dev->u.dsound;

    if (!dso->has_latency) {
        dso->has_latency = true;
        dso->latency = 10000; /* 10 ms */
    }

    hr = CoInitialize (NULL);
    if (FAILED (hr)) {
        dserror_set(errp, hr, "Could not initialize COM");
        return false;
    }

    hr = CoCreateInstance (
        &CLSID_DirectSound,
        NULL,
        CLSCTX_ALL,
        &IID_IDirectSound,
        (void **) &s->dsound
        );
    if (FAILED (hr)) {
        dserror_set(errp, hr, "Could not create DirectSound instance");
        return false;
    }

    hr = IDirectSound_Initialize (s->dsound, NULL);
    if (FAILED (hr)) {
        dserror_set(errp, hr, "Could not initialize DirectSound");
        return false;
    }

    /*
     * Capture is optional: a host with no recording device (no microphone)
     * still plays sound. Without the capture object, an input voice fails to
     * initialize (dsound_init_in).
     */
    hr = CoCreateInstance (
        &CLSID_DirectSoundCapture,
        NULL,
        CLSCTX_ALL,
        &IID_IDirectSoundCapture,
        (void **) &s->dsound_capture
        );
    if (FAILED (hr)) {
        dsound_logerr(hr, "Could not create DirectSoundCapture instance, "
                      "no recording");
        s->dsound_capture = NULL;
    } else {
        hr = IDirectSoundCapture_Initialize (s->dsound_capture, NULL);
        if (FAILED(hr)) {
            dsound_logerr(hr, "Could not initialize DirectSoundCapture, "
                          "no recording");
            IDirectSoundCapture_Release (s->dsound_capture);
            s->dsound_capture = NULL;
        }
    }

    hr = IDirectSound_SetCooperativeLevel (
        s->dsound,
        GetDesktopWindow(),
        DSSCL_PRIORITY
    );
    if (FAILED(hr)) {
        dserror_set(errp, hr, "Could not set cooperative level");
        return false;
    }

    return true;
}

static void audio_dsound_class_init(ObjectClass *klass, const void *data)
{
    AudioBackendClass *b = AUDIO_BACKEND_CLASS(klass);
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    audio_dsound_parent_class = AUDIO_BACKEND_CLASS(object_class_get_parent(klass));

    b->realize = audio_dsound_realize;
    k->max_voices_out = INT_MAX;
    k->max_voices_in = 1;
    k->voice_size_out = sizeof(DSoundVoiceOut);
    k->voice_size_in = sizeof(DSoundVoiceIn);

    k->init_out = dsound_init_out;
    k->fini_out = dsound_fini_out;
    k->write = audio_generic_write;
    k->buffer_get_free = dsound_buffer_get_free;
    k->get_buffer_out = dsound_get_buffer_out;
    k->put_buffer_out = dsound_put_buffer_out;
    k->enable_out = dsound_enable_out;

    k->init_in = dsound_init_in;
    k->fini_in = dsound_fini_in;
    k->read = audio_generic_read;
    k->get_buffer_in = dsound_get_buffer_in;
    k->put_buffer_in = dsound_put_buffer_in;
    k->enable_in = dsound_enable_in;
}

static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_DSOUND,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioDsound),
        .class_init = audio_dsound_class_init,
        .instance_finalize = audio_dsound_finalize,
    },
};

DEFINE_TYPES(audio_types)
module_obj(TYPE_AUDIO_DSOUND);
