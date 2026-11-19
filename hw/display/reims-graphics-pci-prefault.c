/*
 * QEMU reims-graphics device: the pool's pre-fault
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * reims-graphics-pci-prefault.h says what it does. One thread a VM, for
 * every device of the VM's: the populate's calls run on the thread, and
 * the queue, under its lock, holds the pieces not yet started, sorted by
 * address.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "system/kvm.h"
#include "reims-graphics-pci-prefault.h"
#ifdef CONFIG_LINUX
#include "reims-graphics-pci-kvm.h"
#endif
#include "trace.h"

typedef struct ReimsGraphicsPrefaultRange {
    hwaddr gpa;
    uint64_t size;
    int64_t queued; /* get_clock() at the add */
} ReimsGraphicsPrefaultRange;

struct ReimsGraphicsPrefault {
    const ReimsGraphicsPrefaultOps *ops;
    QemuThread thread;
    QemuSemaphore started;
    bool ok;          /* the start's answer, once started is posted */
    QemuMutex lock;
    QemuCond cond;
    GArray *queue;    /* lock: ReimsGraphicsPrefaultRange, sorted by gpa */
    bool busy;        /* lock: a piece is in flight, */
    hwaddr cur_gpa;   /* lock: at cur_gpa, */
    uint64_t cur_size; /* lock: of cur_size bytes, */
    bool cur_stopped; /* lock: and its removal stopped it */
    unsigned paused;  /* lock: the pauses on, which no call starts under */
    bool in_call;     /* lock: the thread is in a call, */
    bool kicked;      /* lock: which a pause has signalled */
    QemuCond idle;    /* broadcast when a call a pause waits for ends */
    /* lock: the totals */
    uint64_t regions, bytes, slices, kicks, errors, stopped, dropped;
    int64_t cpu_ns;
};

static bool reims_graphics_prefault_overlap(hwaddr a, uint64_t a_size,
                                            hwaddr b, uint64_t b_size)
{
    return a < b + b_size && b < a + a_size;
}

/* The calling thread's CPU time, in ns: 0 where the host does not say. */
static int64_t reims_graphics_prefault_cpu_ns(void)
{
#ifdef CLOCK_THREAD_CPUTIME_ID
    struct timespec ts;

    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
        return ts.tv_sec * NANOSECONDS_PER_SECOND + ts.tv_nsec;
    }
#endif
    return 0;
}

/*
 * Populates @r, lowest address first, a slice a call, until it is done, its
 * removal stops it, or a call fails: a failed call leaves the rest of the
 * piece to the guest's first touch, as without the populate.
 */
static void reims_graphics_prefault_region(ReimsGraphicsPrefault *pf,
                                           const ReimsGraphicsPrefaultRange *r)
{
    hwaddr gpa = r->gpa, end = r->gpa + r->size, next;
    int64_t t0 = get_clock(), c0 = reims_graphics_prefault_cpu_ns();
    int64_t s0, slice_ns, slice_max = 0, wall, cpu;
    unsigned slices = 0, kicks = 0;
    int64_t done;
    bool kicked, stopped = false;
    int err = 0;

    while (gpa < end) {
        WITH_QEMU_LOCK_GUARD(&pf->lock) {
            while (pf->paused) {
                qemu_cond_wait(&pf->cond, &pf->lock);
            }
            stopped = pf->cur_stopped;
            pf->in_call = !stopped;
            pf->kicked = false;
        }
        if (stopped) {
            break;
        }
        next = MIN(end, QEMU_ALIGN_DOWN(gpa, REIMS_GRAPHICS_PREFAULT_SLICE) +
                        REIMS_GRAPHICS_PREFAULT_SLICE);
        s0 = get_clock();
        done = pf->ops->populate(gpa, next - gpa);
        slice_ns = get_clock() - s0;
        WITH_QEMU_LOCK_GUARD(&pf->lock) {
            pf->in_call = false;
            kicked = pf->kicked;
            if (pf->paused) {
                qemu_cond_broadcast(&pf->idle);
            }
        }
        slice_max = MAX(slice_max, slice_ns);
        slices++;
        kicks += kicked;
        if (done > 0) {
            gpa += done;
        } else if (done == -EINTR && kicked) {
            /* A pause ended it with nothing done: it starts afresh after. */
        } else {
            err = -done;
            break;
        }
    }
    wall = get_clock() - t0;
    cpu = reims_graphics_prefault_cpu_ns() - c0;
    trace_reims_graphics_prefault_region(r->gpa, r->size, gpa - r->gpa,
                                         slices, kicks, err, stopped);
    trace_reims_graphics_prefault_region_time(r->gpa, t0 - r->queued, wall,
                                              cpu, slice_max);
    WITH_QEMU_LOCK_GUARD(&pf->lock) {
        pf->regions++;
        pf->bytes += gpa - r->gpa;
        pf->slices += slices;
        pf->kicks += kicks;
        pf->errors += err != 0;
        pf->stopped += stopped;
        pf->cpu_ns += cpu;
    }
}

static void *reims_graphics_prefault_thread(void *opaque)
{
    ReimsGraphicsPrefault *pf = opaque;
    ReimsGraphicsPrefaultRange r;
    Error *err = NULL;

    pf->ok = pf->ops->start(&err);
    if (!pf->ok) {
        warn_report_err(err);
    }
    qemu_sem_post(&pf->started);
    if (!pf->ok) {
        return NULL;
    }
    for (;;) {
        WITH_QEMU_LOCK_GUARD(&pf->lock) {
            while (pf->queue->len == 0) {
                qemu_cond_wait(&pf->cond, &pf->lock);
            }
            r = g_array_index(pf->queue, ReimsGraphicsPrefaultRange, 0);
            g_array_remove_index(pf->queue, 0);
            pf->busy = true;
            pf->cur_gpa = r.gpa;
            pf->cur_size = r.size;
            pf->cur_stopped = false;
        }
        reims_graphics_prefault_region(pf, &r);
        WITH_QEMU_LOCK_GUARD(&pf->lock) {
            pf->busy = false;
        }
    }
    return NULL;
}

ReimsGraphicsPrefault *reims_graphics_prefault_get(void)
{
    static ReimsGraphicsPrefault *made;
    static bool tried;
    const ReimsGraphicsPrefaultOps *ops = NULL;
    ReimsGraphicsPrefault *pf;

    if (tried) {
        return made;
    }
    tried = true;
#ifdef CONFIG_LINUX
    if (kvm_enabled()) {
        ops = reims_graphics_kvm_prefault_ops();
    }
#endif
    if (!ops) {
        return NULL;
    }
    pf = g_new0(ReimsGraphicsPrefault, 1);
    pf->ops = ops;
    qemu_sem_init(&pf->started, 0);
    qemu_mutex_init(&pf->lock);
    qemu_cond_init(&pf->cond);
    qemu_cond_init(&pf->idle);
    pf->queue = g_array_new(false, false, sizeof(ReimsGraphicsPrefaultRange));
    /* It lives to the process's exit, waiting for pieces. */
    qemu_thread_create(&pf->thread, "rgfx-prefault",
                       reims_graphics_prefault_thread, pf,
                       QEMU_THREAD_DETACHED);
    qemu_sem_wait(&pf->started);
    if (!pf->ok) {
        g_array_free(pf->queue, true);
        qemu_cond_destroy(&pf->idle);
        qemu_cond_destroy(&pf->cond);
        qemu_mutex_destroy(&pf->lock);
        qemu_sem_destroy(&pf->started);
        g_free(pf);
        return NULL;
    }
    made = pf;
    return made;
}

void reims_graphics_prefault_add(ReimsGraphicsPrefault *pf, hwaddr gpa,
                                 uint64_t size)
{
    ReimsGraphicsPrefaultRange r = {
        .gpa = gpa, .size = size, .queued = get_clock(),
    };
    guint i;

    WITH_QEMU_LOCK_GUARD(&pf->lock) {
        for (i = 0; i < pf->queue->len; i++) {
            if (g_array_index(pf->queue, ReimsGraphicsPrefaultRange,
                              i).gpa > gpa) {
                break;
            }
        }
        g_array_insert_val(pf->queue, i, r);
        qemu_cond_broadcast(&pf->cond);
    }
}

void reims_graphics_prefault_stop(ReimsGraphicsPrefault *pf, hwaddr gpa,
                                  uint64_t size)
{
    ReimsGraphicsPrefaultRange *r;
    guint i = 0;

    WITH_QEMU_LOCK_GUARD(&pf->lock) {
        while (i < pf->queue->len) {
            r = &g_array_index(pf->queue, ReimsGraphicsPrefaultRange, i);
            if (reims_graphics_prefault_overlap(r->gpa, r->size, gpa, size)) {
                trace_reims_graphics_prefault_dropped(r->gpa, r->size);
                pf->dropped++;
                g_array_remove_index(pf->queue, i);
            } else {
                i++;
            }
        }
        if (pf->busy &&
            reims_graphics_prefault_overlap(pf->cur_gpa, pf->cur_size,
                                            gpa, size)) {
            pf->cur_stopped = true;
        }
    }
}

void reims_graphics_prefault_pause(ReimsGraphicsPrefault *pf)
{
    int64_t t0 = get_clock();
    unsigned kicks = 0;
    bool in_call;

    WITH_QEMU_LOCK_GUARD(&pf->lock) {
        pf->paused++;
        in_call = pf->in_call;
        while (pf->in_call) {
            /*
             * A signal that lands before the call is in the kernel ends
             * nothing: each millisecond the call goes on, it is sent again.
             */
            pf->kicked = true;
            pf->ops->kick();
            kicks++;
            qemu_cond_timedwait(&pf->idle, &pf->lock, 1);
        }
    }
    trace_reims_graphics_prefault_pause(in_call, kicks, get_clock() - t0);
}

void reims_graphics_prefault_resume(ReimsGraphicsPrefault *pf)
{
    WITH_QEMU_LOCK_GUARD(&pf->lock) {
        assert(pf->paused > 0);
        if (--pf->paused == 0) {
            qemu_cond_broadcast(&pf->cond);
        }
    }
}

void reims_graphics_prefault_report(ReimsGraphicsPrefault *pf)
{
    WITH_QEMU_LOCK_GUARD(&pf->lock) {
        trace_reims_graphics_prefault_totals(pf->regions, pf->bytes,
                                             pf->slices, pf->kicks, pf->errors,
                                             pf->stopped, pf->dropped,
                                             pf->cpu_ns);
    }
}
