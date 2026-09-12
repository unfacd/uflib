/**
 * @file adt_minheap.c
 * @brief Single-threaded binary min-heap over a flat, geometrically-grown array.
 *
 * Replacement for the pre-2026 page-mapped (`mmap`/`munmap`) min-heap.  The
 * working set is a single `realloc` buffer of `MinHeapEntry` records; there is
 * no mapping-table indirection, no per-growth page zeroing, and no
 * sub-`munmap` of a multi-page region.  Sift-up and sift-down move a single
 * "hole" down/up a root-leaf path and write the displaced record once at the
 * end, rather than swapping entry pairs.
 *
 * Two key modes are supported:
 *   - HEAP_MODE_PTR: keys/values are opaque `void *`, ordered by a caller
 *     comparator (`MinHeapCmpFn`).  The comparator may return any negative /
 *     zero / positive magnitude; sift-down treats `> 0` as "greater", never
 *     `== 1`.
 *   - HEAP_MODE_I64: keys are inline `int64_t`, compared by direct integer
 *     subtract-sign — no function pointer on the hot path.
 *
 * Not thread-safe: one owner thread per heap.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uflib/adt/adt_minheap.h>

/* ── Internal key-mode tags ────────────────────────────────────────────── */

enum { HEAP_MODE_PTR = 0, HEAP_MODE_I64 = 1 };

/* ── Capacity policy ───────────────────────────────────────────────────── */

enum {
    HEAP_DEFAULT_CAPACITY = 16,
    /* 2^30 entries (~16 GiB) is the ceiling.  sGrowTo refuses to double past
     * this, so `cap *= 2` can never overflow a signed int. */
    HEAP_MAX_CAPACITY = (1 << 30)
};

/* ── Storage ───────────────────────────────────────────────────────────── */

typedef struct MinHeapEntry {
    union {
        void    *key;      /* HEAP_MODE_PTR key */
        int64_t  key_i64;  /* HEAP_MODE_I64 key */
    } k;
    void *value;
} MinHeapEntry;

struct MinHeap {
    MinHeapCmpFn  compare_func;  /* unused in HEAP_MODE_I64 */
    MinHeapEntry  *entries;
    int          size;          /* live entries */
    int          cap;           /* allocated entries */
    int          mode;          /* HEAP_MODE_PTR or HEAP_MODE_I64 */
};

/* ── Comparators (public) ──────────────────────────────────────────────── */

int MinHeapCompareIntKeys(const void *key1, const void *key2)
{
    int a = *(const int *)key1;
    int b = *(const int *)key2;
    return (a > b) - (a < b);
}

int MinHeapCompareLongLongKeys(const void *key1, const void *key2)
{
    long long a = *(const long long *)key1;
    long long b = *(const long long *)key2;
    return (a > b) - (a < b);
}

int MinHeapComparePtrAsInteger(const void *key1, const void *key2)
{
    uintptr_t a = (uintptr_t)key1;
    uintptr_t b = (uintptr_t)key2;
    return (a > b) - (a < b);
}

/* ── Growth ────────────────────────────────────────────────────────────── */

static int
sGrowTo(MinHeap *h, int needed)
{
    int cap = h->cap > 0 ? h->cap : HEAP_DEFAULT_CAPACITY;

    if (needed < 0)
        return MINHEAP_ERR_INVAL;

    while (cap < needed) {
        if (cap >= HEAP_MAX_CAPACITY)
            return MINHEAP_ERR_OOM;  /* next doubling would overflow int */
        cap *= 2;
    }

    MinHeapEntry *p = (MinHeapEntry *)realloc(h->entries, (size_t)cap * sizeof(*p));
    if (!p)
        return MINHEAP_ERR_OOM;

    h->entries = p;
    h->cap = cap;
    return MINHEAP_OK;
}

static MinHeap *
sCreateCommon(int initial_capacity, MinHeapCmpFn cmp, int mode)
{
    if (initial_capacity < 0)
        return NULL;

    MinHeap *h = (MinHeap *)calloc(1, sizeof(*h));
    if (!h)
        return NULL;

    h->compare_func = cmp;
    h->mode = mode;
    h->size = 0;
    h->cap = 0;
    h->entries = NULL;

    int want = initial_capacity > 0 ? initial_capacity : HEAP_DEFAULT_CAPACITY;
    if (sGrowTo(h, want) != MINHEAP_OK) {
        free(h);
        return NULL;
    }
    return h;
}

/* ── Page geometry (informational; flat buffer, not pages) ─────────────── */

static int
sPageSize(void)
{
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (int)ps : 4096;
}

/* ── Ordering helpers ──────────────────────────────────────────────────── */

static int
sKeyLessPtr(const MinHeap *h, void *ka, void *kb)
{
    return h->compare_func(ka, kb) < 0;
}

/* ── Sift (hole-moving; write-once at the end) ─────────────────────────── */

static void
sSiftUpPtr(MinHeap *h, int hole, void *key, void *value)
{
    MinHeapEntry *e = h->entries;
    while (hole > 0) {
        int parent = (hole - 1) >> 1;
        if (!sKeyLessPtr(h, key, e[parent].k.key))
            break;
        e[hole] = e[parent];
        hole = parent;
    }
    e[hole].k.key = key;
    e[hole].value = value;
}

static void
sSiftUpI64(MinHeap *h, int hole, int64_t key, void *value)
{
    MinHeapEntry *e = h->entries;
    while (hole > 0) {
        int parent = (hole - 1) >> 1;
        if (key >= e[parent].k.key_i64)
            break;
        e[hole] = e[parent];
        hole = parent;
    }
    e[hole].k.key_i64 = key;
    e[hole].value = value;
}

static void
sSiftDownPtr(MinHeap *h, int hole, MinHeapEntry last, int n)
{
    MinHeapEntry *e = h->entries;
    MinHeapCmpFn cmp = h->compare_func;
    for (;;) {
        int left = (hole << 1) + 1;
        if (left >= n)
            break;
        int child = left;
        int right = left + 1;
        if (right < n && cmp(e[right].k.key, e[left].k.key) < 0)
            child = right;
        if (cmp(e[child].k.key, last.k.key) >= 0)
            break;
        e[hole] = e[child];
        hole = child;
    }
    e[hole] = last;
}

static void
sSiftDownI64(MinHeap *h, int hole, MinHeapEntry last, int n)
{
    MinHeapEntry *e = h->entries;
    for (;;) {
        int left = (hole << 1) + 1;
        if (left >= n)
            break;
        int child = left;
        int right = left + 1;
        if (right < n && e[right].k.key_i64 < e[left].k.key_i64)
            child = right;
        if (e[child].k.key_i64 >= last.k.key_i64)
            break;
        e[hole] = e[child];
        hole = child;
    }
    e[hole] = last;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

MinHeap *
MinHeapCreate(int initial_capacity, MinHeapCmpFn comp_func)
{
    if (!comp_func)
        comp_func = MinHeapCompareIntKeys;
    return sCreateCommon(initial_capacity, comp_func, HEAP_MODE_PTR);
}

MinHeap *
MinHeapCreateI64(int initial_capacity)
{
    return sCreateCommon(initial_capacity, NULL, HEAP_MODE_I64);
}

void
MinHeapDestroy(MinHeap *h)
{
    if (!h)
        return;
    free(h->entries);
    h->entries = NULL;  /* defensive: a stale handle can no longer reach the array */
    h->size = 0;
    h->cap = 0;
    free(h);
}

/* ── Introspection ─────────────────────────────────────────────────────── */

int
MinHeapSize(const MinHeap *h)
{
    return h ? h->size : 0;
}

int
MinHeapCapacity(const MinHeap *h)
{
    return h ? h->cap : 0;
}

int
MinHeapPageSize(void)
{
    return sPageSize();
}

int
MinHeapEntriesPerPage(void)
{
    int epp = sPageSize() / (int)sizeof(MinHeapEntry);
    return epp > 0 ? epp : 1;
}

/* ── Capacity ──────────────────────────────────────────────────────────── */

int
MinHeapReserve(MinHeap *h, int n)
{
    if (!h || n < 0)
        return MINHEAP_ERR_INVAL;
    if (n <= h->cap)
        return MINHEAP_OK;
    return sGrowTo(h, n);
}

void
MinHeapClear(MinHeap *h)
{
    if (!h)
        return;
    h->size = 0;
}

/* ── Insert ────────────────────────────────────────────────────────────── */

int
MinHeapInsert(MinHeap *h, void *key, void *value)
{
    if (!h || h->mode != HEAP_MODE_PTR)
        return MINHEAP_ERR_INVAL;
    if (h->size >= h->cap) {
        int rc = sGrowTo(h, h->size + 1);
        if (rc != MINHEAP_OK)
            return rc;
    }
    sSiftUpPtr(h, h->size, key, value);
    h->size++;
    return MINHEAP_OK;
}

int
MinHeapInsertI64(MinHeap *h, int64_t key, void *value)
{
    if (!h || h->mode != HEAP_MODE_I64)
        return MINHEAP_ERR_INVAL;
    if (h->size >= h->cap) {
        int rc = sGrowTo(h, h->size + 1);
        if (rc != MINHEAP_OK)
            return rc;
    }
    sSiftUpI64(h, h->size, key, value);
    h->size++;
    return MINHEAP_OK;
}

/* ── Peek ──────────────────────────────────────────────────────────────── */

int
MinHeapMin(const MinHeap *h, void **key, void **value)
{
    if (!h || h->size <= 0 || h->mode != HEAP_MODE_PTR)
        return 0;
    if (key)
        *key = h->entries[0].k.key;
    if (value)
        *value = h->entries[0].value;
    return 1;
}

int
MinHeapPeek(const MinHeap *h, void **key, void **value)
{
    return MinHeapMin(h, key, value);
}

int
MinHeapMinI64(const MinHeap *h, int64_t *key, void **value)
{
    if (!h || h->size <= 0 || h->mode != HEAP_MODE_I64)
        return 0;
    if (key)
        *key = h->entries[0].k.key_i64;
    if (value)
        *value = h->entries[0].value;
    return 1;
}

/* ── Remove ────────────────────────────────────────────────────────────── */

int
MinHeapDelmin(MinHeap *h, void **key, void **value)
{
    if (!h || h->size <= 0 || h->mode != HEAP_MODE_PTR)
        return 0;
    if (key)
        *key = h->entries[0].k.key;
    if (value)
        *value = h->entries[0].value;
    h->size--;
    if (h->size > 0)
        sSiftDownPtr(h, 0, h->entries[h->size], h->size);
    return 1;
}

int
MinHeapDelminI64(MinHeap *h, int64_t *key, void **value)
{
    if (!h || h->size <= 0 || h->mode != HEAP_MODE_I64)
        return 0;
    if (key)
        *key = h->entries[0].k.key_i64;
    if (value)
        *value = h->entries[0].value;
    h->size--;
    if (h->size > 0)
        sSiftDownI64(h, 0, h->entries[h->size], h->size);
    return 1;
}

int
MinHeapPop(MinHeap *h, void **value)
{
    if (!h || h->size <= 0)
        return 0;
    if (h->mode == HEAP_MODE_I64)
        return MinHeapDelminI64(h, NULL, value);
    return MinHeapDelmin(h, NULL, value);
}

/* ── Iterate ───────────────────────────────────────────────────────────── */

size_t
MinHeapForeach(const MinHeap *h,
             void (*func)(void *key, void *value, void *ctx),
             ClientContextData *ctx_data)
{
    if (!h || !func || h->size <= 0)
        return 0;

    int n = h->size;
    for (int i = 0; i < n; i++) {
        void *key = (h->mode == HEAP_MODE_I64)
                        ? (void *)(intptr_t)h->entries[i].k.key_i64
                        : h->entries[i].k.key;
        func(key, h->entries[i].value, ctx_data);
    }
    return (size_t)n;
}

/* ── Introspection (JSON) ──────────────────────────────────────────────── */

PUBLIC_API BufferDescriptor *
MinHeapDescribe(const MinHeap *h, BufferDescriptor *provided)
{
    if (!provided) {
        provided = calloc(1, sizeof(BufferDescriptor));
        if (!provided)
            return NULL;
        BufferDescriptorInit(provided, 256);
    }

    if (!h) {
        BufferDescriptorAppendFormatted(provided, "{\"error\":\"null handle\"}\n");
        return provided;
    }

    double load_factor = (h->cap > 0) ? (double)h->size / (double)h->cap : 0.0;

    BufferDescriptorAppendFormatted(provided,
        "{\"size\":%d,\"capacity\":%d,\"mode\":\"%s\",\"load_factor\":%.4f,"
        "\"has_comparator\":%s",
        h->size, h->cap,
        h->mode == HEAP_MODE_I64 ? "i64" : "ptr",
        load_factor,
        h->compare_func ? "true" : "false");

    BufferDescriptorAppendFormatted(provided, ",\"entries\":[");
    int emitted = 0;
    for (int i = 0; i < h->size; i++) {
        BufferDescriptorAppendFormatted(provided, "%s{\"index\":%d,",
            emitted ? "," : "", i);
        if (h->mode == HEAP_MODE_I64) {
            BufferDescriptorAppendFormatted(provided, "\"key\":%" PRId64,
                (int64_t)h->entries[i].k.key_i64);
        } else {
            void *key = h->entries[i].k.key;
            if (key)
                BufferDescriptorAppendFormatted(provided,
                    "\"key\":\"0x%" PRIxPTR "\"", (uintptr_t)key);
            else
                BufferDescriptorAppendFormatted(provided, "\"key\":null");
        }
        void *value = h->entries[i].value;
        if (value)
            BufferDescriptorAppendFormatted(provided,
                ",\"value\":\"0x%" PRIxPTR "\"", (uintptr_t)value);
        else
            BufferDescriptorAppendFormatted(provided, ",\"value\":null");
        BufferDescriptorAppendFormatted(provided, "}");
        emitted = 1;
    }
    BufferDescriptorAppendFormatted(provided, "]}\n");

    return provided;
}
