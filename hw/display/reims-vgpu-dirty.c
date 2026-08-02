/*
 * reims-vgpu: guest-write tracking over the hypervisor dirty bitmap.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/rcu.h"
#include "exec/target_page.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/physmem.h"
#include "system/ram_addr.h"

#include "reims-vgpu-dirty.h"

/*
 * Contiguous slice of the tracked GPA window that translates into one
 * MemoryRegion. Guest RAM is a handful of regions, so one translation per
 * slice replaces one per page — the same reason reims_vgpu_*_map_pages walks
 * maximal runs.
 */
typedef struct ReimsVgpuDirtySlice {
    MemoryRegion *mr;       /* NULL => not RAM; every page here reads written */
    uint64_t gpa;           /* first GPA covered */
    uint64_t len;           /* bytes covered */
    ram_addr_t ram_addr;    /* ram_addr_t of `gpa` */
    hwaddr offset;          /* offset of `gpa` within mr */
} ReimsVgpuDirtySlice;

/* A tracked page set: one mapping incarnation's guest storage. */
typedef struct ReimsVgpuDirtySet {
    uint64_t *pages;        /* sorted, deduplicated, page-aligned */
    /*
     * Parallel to `pages`: the value `gen` took at the harvest that last saw
     * this page written, or 0 for a page never seen written. A set-level
     * generation can only say "something in here moved", which forces a reader
     * holding a whole-surface copy to discard all of it; this is what lets it
     * discard exactly the pages that moved.
     */
    uint64_t *page_gen;
    size_t count;
    uint64_t page_size;
    uint64_t gen;           /* 0 until armed */
    uint64_t arm_at;        /* harvest count at which gen may leave 0 */
} ReimsVgpuDirtySet;

struct ReimsVgpuDirty {
    QemuMutex lock;
    GHashTable *sets;       /* token -> ReimsVgpuDirtySet* */
    uint64_t next_token;
    /*
     * Regions this device turned DIRTY_MEMORY_VGA logging on for, referenced
     * so the pointers stay valid until we turn it back off. Logging stays on
     * for the device's life: toggling it per surface re-protects the whole
     * region each time, and the guest's entire working set would refault for a
     * saving this device never collects.
     */
    GHashTable *logged;     /* MemoryRegion* -> itself */
    /*
     * Monotone hull of every GPA ever tracked. Only used to cut translation
     * slices, never to clear bits — the clear is per written page, so a hull
     * that grows to most of guest RAM costs one extra slice walk and nothing
     * else. It never shrinks because a rescan on untrack would cost more than
     * the walk it saves.
     */
    uint64_t lo;
    uint64_t hi;            /* exclusive; 0 means nothing tracked */
    uint64_t harvests;
    /*
     * Generation reads since the last harvest. A harvest nothing has consumed
     * since the previous one cannot tell any reader something it does not
     * already know, so it is skipped — which collapses a burst of doorbell
     * writes into one sync.
     */
    uint64_t reads_since_harvest;
};

/*
 * Bound on translation slices per harvest. Guest RAM is a handful of
 * MemoryRegions (below-4G / above-4G on x86, one on arm); a window needing
 * more than this is not a shape this device produces, and "written" is the
 * safe reading of a window it could not cut.
 */
#define REIMS_VGPU_DIRTY_MAX_SLICES 64

static void reims_vgpu_dirty_set_free(gpointer p)
{
    ReimsVgpuDirtySet *s = p;

    g_free(s->pages);
    g_free(s->page_gen);
    g_free(s);
}

ReimsVgpuDirty *reims_vgpu_dirty_new(void)
{
    ReimsVgpuDirty *d = g_new0(ReimsVgpuDirty, 1);

    qemu_mutex_init(&d->lock);
    d->sets = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                                    reims_vgpu_dirty_set_free);
    d->logged = g_hash_table_new(NULL, NULL);
    return d;
}

void reims_vgpu_dirty_free(ReimsVgpuDirty *d)
{
    GHashTableIter it;
    gpointer key, val;

    if (!d) {
        return;
    }
    /* BQL: memory_region_set_log is a MemoryRegion transaction. */
    g_hash_table_iter_init(&it, d->logged);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        MemoryRegion *mr = key;

        memory_region_set_log(mr, false, DIRTY_MEMORY_VGA);
        memory_region_unref(mr);
    }
    g_hash_table_destroy(d->logged);
    g_hash_table_destroy(d->sets);
    qemu_mutex_destroy(&d->lock);
    g_free(d);
}

static int reims_vgpu_dirty_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    return x < y ? -1 : (x > y ? 1 : 0);
}

/*
 * One guest page a harvest found written, with the geometry it was tracked at.
 * The size travels with the page because the clear must cover the whole guest
 * page and the two pathways do not share a page shift.
 */
typedef struct ReimsVgpuDirtyWritten {
    uint64_t gpa;
    uint64_t page_size;
} ReimsVgpuDirtyWritten;

static int reims_vgpu_dirty_cmp_written(gconstpointer a, gconstpointer b)
{
    const ReimsVgpuDirtyWritten *x = a;
    const ReimsVgpuDirtyWritten *y = b;

    if (x->gpa != y->gpa) {
        return x->gpa < y->gpa ? -1 : 1;
    }
    /* Larger page first, so a duplicate of the same base collapses into the
     * run the wider page opens rather than truncating it. */
    return x->page_size > y->page_size ? -1 : (x->page_size < y->page_size);
}

uint64_t reims_vgpu_dirty_track(ReimsVgpuDirty *d, const uint64_t *gpas,
                                size_t count, size_t page_size)
{
    ReimsVgpuDirtySet *s;
    uint64_t *pages;
    uint64_t token;
    size_t i, n;

    if (!d || !gpas || count == 0 || page_size == 0 ||
        (page_size & (page_size - 1)) != 0) {
        return 0;
    }
    /*
     * The dirty bitmap is indexed in target pages, so a guest page smaller
     * than one cannot be asked about. Both product pathways are the other way
     * round (4 KiB x86 == target, 16 KiB arm64e > target), and answering for a
     * granularity the bitmap does not have would be a guess.
     */
    if (page_size < qemu_target_page_size()) {
        return 0;
    }

    pages = g_new(uint64_t, count);
    for (i = 0; i < count; i++) {
        pages[i] = gpas[i] & ~((uint64_t)page_size - 1);
    }
    qsort(pages, count, sizeof(*pages), reims_vgpu_dirty_cmp_u64);
    n = 0;
    for (i = 0; i < count; i++) {
        if (n == 0 || pages[n - 1] != pages[i]) {
            pages[n++] = pages[i];
        }
    }

    s = g_new0(ReimsVgpuDirtySet, 1);
    s->pages = pages;
    s->page_gen = g_new0(uint64_t, n);
    s->count = n;
    s->page_size = page_size;

    qemu_mutex_lock(&d->lock);
    /*
     * One harvest, and a second only if that harvest has to turn logging on.
     *
     * This used to be an unconditional two, because "the first harvest after
     * this call is the one that may still be turning logging on for these
     * pages, so writes older than it were never recorded". That reason is
     * right, and it is a reason about the *first* set to name a region — not
     * about every set. Guest RAM is one MemoryRegion, so once any surface has
     * been tracked, logging is already on for every later one, no harvest ever
     * enables anything, and the second harvest was waiting for something that
     * had already happened.
     *
     * It is not free to wait. While the set is unarmed its generation reads
     * back 0, `HostOps::guest_write_gen` maps that to "cannot tell", the
     * type-11 LOAD elision refuses with `t11_gw_ref_no_stamp`, and every draw
     * onto that surface pays a whole-frame seed read out of guest pages plus a
     * whole-frame staging upload. The window is counted in harvests and
     * harvests are driven by guest doorbells, so on a quiet desktop it lasts as
     * long as the guest stays quiet — which is exactly when a draw arriving
     * into it is a visible hitch (measured at 12-65 ms per draw, against
     * 0.2 ms driven).
     *
     * The second harvest is still taken when it is owed: `reims_vgpu_dirty_harvest`
     * pushes every set still waiting when it had to enable logging, so a set
     * created before the region was logged arms exactly as late as it used to.
     */
    s->arm_at = d->harvests + 1;
    if (d->hi == 0) {
        d->lo = pages[0];
        d->hi = pages[n - 1] + page_size;
    } else {
        if (pages[0] < d->lo) {
            d->lo = pages[0];
        }
        if (pages[n - 1] + page_size > d->hi) {
            d->hi = pages[n - 1] + page_size;
        }
    }
    d->next_token++;
    token = d->next_token;
    g_hash_table_insert(d->sets, g_memdup2(&token, sizeof(token)), s);
    qemu_mutex_unlock(&d->lock);
    return token;
}

void reims_vgpu_dirty_untrack(ReimsVgpuDirty *d, uint64_t token)
{
    if (!d || token == 0) {
        return;
    }
    qemu_mutex_lock(&d->lock);
    g_hash_table_remove(d->sets, &token);
    qemu_mutex_unlock(&d->lock);
}

uint64_t reims_vgpu_dirty_gen(ReimsVgpuDirty *d, uint64_t token)
{
    ReimsVgpuDirtySet *s;
    uint64_t gen = 0;

    if (!d || token == 0) {
        return 0;
    }
    qemu_mutex_lock(&d->lock);
    s = g_hash_table_lookup(d->sets, &token);
    if (s) {
        gen = s->gen;
    }
    d->reads_since_harvest++;
    qemu_mutex_unlock(&d->lock);
    return gen;
}

int64_t reims_vgpu_dirty_written_since(ReimsVgpuDirty *d, uint64_t token,
                                       uint64_t since_gen, uint64_t *out,
                                       size_t max)
{
    ReimsVgpuDirtySet *s;
    int64_t found = 0;
    size_t p;

    if (!d || token == 0 || out == NULL) {
        return -1;
    }
    qemu_mutex_lock(&d->lock);
    s = g_hash_table_lookup(d->sets, &token);
    /*
     * `since_gen == 0` is a caller that never recorded a readable observation,
     * and `s->gen == 0` is a set still inside its startup window. Neither can
     * be compared against a page stamp, and both mean the same thing to the
     * caller as an unknown token does.
     */
    if (!s || s->gen == 0 || since_gen == 0) {
        qemu_mutex_unlock(&d->lock);
        return -1;
    }
    for (p = 0; p < s->count; p++) {
        if (s->page_gen[p] <= since_gen) {
            continue;
        }
        if ((size_t)found == max) {
            /* Truncation would read as "these pages and no others". */
            qemu_mutex_unlock(&d->lock);
            return -1;
        }
        out[found++] = s->pages[p];
    }
    /*
     * Counted like reims_vgpu_dirty_gen(): a caller asking this question is a
     * consumer of the report, so the next harvest has something to tell it and
     * must not be skipped.
     */
    d->reads_since_harvest++;
    qemu_mutex_unlock(&d->lock);
    return found;
}

/*
 * Cut [lo, hi) into MemoryRegion-contiguous slices. Returns the slice count,
 * or -1 when the window needs more slices than this device produces.
 */
static int reims_vgpu_dirty_slice(uint64_t lo, uint64_t hi,
                                  ReimsVgpuDirtySlice *out, int max)
{
    uint64_t cur = lo;
    int n = 0;

    RCU_READ_LOCK_GUARD();
    while (cur < hi) {
        hwaddr xlat, plen = hi - cur;
        MemoryRegion *mr;

        mr = address_space_translate(&address_space_memory, cur, &xlat, &plen,
                                     true, MEMTXATTRS_UNSPECIFIED);
        if (plen == 0 || n == max) {
            return -1;
        }
        out[n].mr = (mr && memory_region_is_ram(mr)) ? mr : NULL;
        out[n].gpa = cur;
        out[n].len = plen;
        out[n].offset = xlat;
        out[n].ram_addr = out[n].mr
            ? memory_region_get_ram_addr(out[n].mr) + xlat
            : 0;
        n++;
        cur += plen;
    }
    return n;
}

/* Which slice holds `gpa`, or -1. Slices are few and ordered, so a linear
 * scan beats any structure these counts would justify. */
static int reims_vgpu_dirty_slice_of(const ReimsVgpuDirtySlice *slices, int n,
                                     uint64_t gpa)
{
    int i;

    for (i = 0; i < n; i++) {
        if (gpa >= slices[i].gpa && gpa - slices[i].gpa < slices[i].len) {
            return i;
        }
    }
    return -1;
}

/*
 * Was any target page of the guest page at `gpa` written?
 *
 * A pure read of the bitmap, deliberately: the answer must be the same for
 * every set holding the page, so nothing is consumed here. Clearing happens
 * once, after every set has been evaluated.
 */
static bool reims_vgpu_dirty_page_written(const ReimsVgpuDirtySlice *sl,
                                          uint64_t gpa, uint64_t page_size)
{
    size_t target = qemu_target_page_size();
    ram_addr_t base = sl->ram_addr + (gpa - sl->gpa);
    uint64_t off;

    for (off = 0; off < page_size; off += target) {
        if (physical_memory_get_dirty_flag(base + off, DIRTY_MEMORY_VGA)) {
            return true;
        }
    }
    return false;
}

void reims_vgpu_dirty_harvest(ReimsVgpuDirty *d)
{
    ReimsVgpuDirtySlice slices[REIMS_VGPU_DIRTY_MAX_SLICES];
    g_autoptr(GArray) written = NULL;
    g_autoptr(GArray) hit = NULL;
    GHashTableIter it;
    gpointer key, val;
    uint64_t lo, hi;
    int n, i;
    bool enabled_any = false;
    guint w;

    if (!d) {
        return;
    }

    qemu_mutex_lock(&d->lock);
    lo = d->lo;
    hi = d->hi;
    if (hi == 0 || d->reads_since_harvest == 0) {
        qemu_mutex_unlock(&d->lock);
        return;
    }
    qemu_mutex_unlock(&d->lock);

    /*
     * Pass one names the regions. Enabling logging on a new one is a
     * MemoryRegion transaction that rebuilds the flat view, so it happens
     * outside the RCU section that named it, and the window is cut again
     * against the view the transaction produced.
     */
    n = reims_vgpu_dirty_slice(lo, hi, slices, REIMS_VGPU_DIRTY_MAX_SLICES);
    for (i = 0; i < n; i++) {
        MemoryRegion *mr = slices[i].mr;

        if (mr && !g_hash_table_contains(d->logged, mr)) {
            memory_region_ref(mr);
            g_hash_table_add(d->logged, mr);
            memory_region_set_log(mr, true, DIRTY_MEMORY_VGA);
            enabled_any = true;
        }
    }
    if (enabled_any) {
        n = reims_vgpu_dirty_slice(lo, hi, slices, REIMS_VGPU_DIRTY_MAX_SLICES);
    }

    /* One sync for every logged region, then only reads. */
    memory_global_dirty_log_sync(false);

    written = g_array_new(FALSE, FALSE, sizeof(ReimsVgpuDirtyWritten));
    /*
     * Indices of the current set's written pages. Held across the generation
     * update because the value to stamp them with is the generation this
     * harvest produces, which is not known until every page has been read.
     * Reused between sets so the harvest allocates once, not once per mapping.
     */
    hit = g_array_new(FALSE, FALSE, sizeof(size_t));
    qemu_mutex_lock(&d->lock);
    d->harvests++;
    d->reads_since_harvest = 0;
    /*
     * This harvest turned logging on for a region, so the sync above is the
     * first one that saw it and writes older than it were never recorded. Every
     * set still inside its arming window is therefore not yet covered by a
     * complete sync, whichever region it named: push it to the next harvest.
     *
     * This is what lets `reims_vgpu_dirty_track` arm at +1 instead of +2. The
     * +2 was paying for this case on every set; this pays for it on the sets it
     * actually applies to, which after the first surface of a boot is none.
     *
     * The test is `s->gen == 0` — never armed — and not `d->harvests <
     * s->arm_at`. A set created at harvest H carries `arm_at = H + 1`, and
     * `d->harvests` is already H + 1 here, so the second test is false for
     * exactly the set that needs pushing. `s->gen` leaves 0 only in the arming
     * block below and never returns to it, so it is the durable spelling of
     * "this set has not been armed yet".
     */
    if (enabled_any) {
        g_hash_table_iter_init(&it, d->sets);
        while (g_hash_table_iter_next(&it, &key, &val)) {
            ReimsVgpuDirtySet *s = val;

            if (s->gen == 0) {
                s->arm_at = d->harvests + 1;
            }
        }
    }
    g_hash_table_iter_init(&it, d->sets);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ReimsVgpuDirtySet *s = val;
        bool any = false;
        size_t p;

        g_array_set_size(hit, 0);
        for (p = 0; p < s->count; p++) {
            uint64_t gpa = s->pages[p];
            int si = reims_vgpu_dirty_slice_of(slices, n, gpa);

            /*
             * Outside the window this harvest cut, or not RAM this device can
             * read a bitmap for. Both mean the host cannot say the page is
             * unwritten, and that reads as written — per page as well as for
             * the set, so a per-page reader is no less conservative than the
             * generation is.
             */
            if (si < 0 || !slices[si].mr) {
                any = true;
                g_array_append_val(hit, p);
                continue;
            }
            if (reims_vgpu_dirty_page_written(&slices[si], gpa, s->page_size)) {
                ReimsVgpuDirtyWritten rec = { gpa, s->page_size };

                any = true;
                g_array_append_val(hit, p);
                /* Recorded for the clear pass, which must not run until every
                 * set has read the bit — otherwise the first set harvested
                 * would consume the report of every set sharing the page. */
                g_array_append_val(written, rec);
            }
        }
        if (d->harvests < s->arm_at) {
            /* Startup window: an absence of reports says nothing about the
             * guest yet, so the generation stays unreadable. */
            s->gen = 0;
        } else if (s->gen == 0) {
            s->gen = 1;
        } else if (any) {
            s->gen++;
        }
        /*
         * Stamp after the generation, and only once it is readable. A page
         * stamped during the startup window would carry a generation no reader
         * can have recorded, and would then read as written forever.
         */
        if (s->gen != 0) {
            guint h;

            for (h = 0; h < hit->len; h++) {
                s->page_gen[g_array_index(hit, size_t, h)] = s->gen;
            }
        }
    }
    qemu_mutex_unlock(&d->lock);

    /*
     * Clear only the pages that came back written. Clearing re-protects, so
     * clearing the whole tracked window would make every page the guest has
     * mapped refault after every harvest; clearing what was written costs
     * exactly one refault per page the guest writes again, which is the work
     * the report is worth.
     *
     * Exactly-adjacent pages merge into one call because each call is a
     * hypervisor round trip. Nothing is widened: a merged range covers only
     * pages that were themselves written.
     */
    g_array_sort(written, reims_vgpu_dirty_cmp_written);
    w = 0;
    while (w < written->len) {
        ReimsVgpuDirtyWritten first = g_array_index(written, ReimsVgpuDirtyWritten, w);
        int si = reims_vgpu_dirty_slice_of(slices, n, first.gpa);
        uint64_t end = first.gpa + first.page_size;

        while (w + 1 < written->len) {
            ReimsVgpuDirtyWritten next =
                g_array_index(written, ReimsVgpuDirtyWritten, w + 1);

            if (next.gpa + next.page_size <= end) {
                /* Same page recorded by a second set, or a smaller page
                 * already inside the run. */
                w++;
                continue;
            }
            if (next.gpa != end ||
                reims_vgpu_dirty_slice_of(slices, n, next.gpa) != si) {
                break;
            }
            end = next.gpa + next.page_size;
            w++;
        }
        if (si >= 0 && slices[si].mr) {
            memory_region_reset_dirty(
                slices[si].mr, slices[si].offset + (first.gpa - slices[si].gpa),
                end - first.gpa, DIRTY_MEMORY_VGA);
        }
        w++;
    }
}
