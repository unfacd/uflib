/**
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

/**
 * @file adt_minheap.h
 * @brief Single-threaded binary min-heap (priority queue) — public API.
 *
 * A classic binary min-heap over a flat, geometrically-grown array.  It is
 * an ADT, not a concurrent structure: exactly one owner thread may touch a
 * given heap at a time (one heap per thread, or an external lock around the
 * whole API).
 *
 * Two key modes exist:
 *
 * - **Pointer keys** — MinHeapCreate(): keys and values are opaque `void *`.
 *   The heap stores the pointers it is given; it never copies or frees the
 *   pointed-to data, so keys and values must remain valid until the entry is
 *   removed, the heap is cleared, or the heap is destroyed.
 * - **Inline int64 keys** — MinHeapCreateI64(): keys are `int64_t` values
 *   stored directly in the entry.  Comparisons are a direct integer
 *   subtract-sign on that path, so no function pointer is ever called.  This
 *   is the hot path for timer / deadline / Dijkstra heaps.
 *
 * The comparator contract is *any* negative / zero / positive value (not
 * just -1/0/1).  A comparator that returns -2/+2 sorts correctly.
 *
 * Error handling: MinHeapCreate()/MinHeapCreateI64() return NULL on failure
 * (OOM, invalid capacity).  Mutating entry points (MinHeapInsert, MinHeapReserve)
 * return MINHEAP_OK / MINHEAP_ERR_OOM / MINHEAP_ERR_INVAL rather than aborting.  The
 * query/remove entry points (MinHeapMin, MinHeapDelmin, MinHeapPop) return 1 on
 * success and 0 when the heap is empty, the handle is NULL, or the key mode
 * does not match — 0 is a boolean "no element", not an error code.
 */

#ifndef UFLIB_ADT_ADT_MINHEAP_H
#define UFLIB_ADT_ADT_MINHEAP_H

#include <stddef.h>
#include <stdint.h>

#include <uflib/uflib_defs.h>
#include <uflib/main_types.h>
#include <uflib/adt/adt_minheap_type.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <uflib/logger/logger_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Status codes ──────────────────────────────────────────────────────── */

enum {
    MINHEAP_OK        =  0,   /**< Success. */
    MINHEAP_ERR_OOM   = -1,   /**< Allocation failure (or capacity overflow). */
    MINHEAP_ERR_INVAL = -2    /**< Invalid argument (NULL handle, wrong key mode). */
};

/* ── Comparators ───────────────────────────────────────────────────────── */

/**
 * @brief Compare two `int` keys.  Dereferences the pointers as `const int *`.
 */
PUBLIC_API int MinHeapCompareIntKeys(const void *key1, const void *key2);

/**
 * @brief Compare two `long long` keys.  Dereferences the pointers as
 *        `const long long *`.
 *
 * This is the corrected contract: it dereferences `long long *` keys.  The
 * pre-2026 implementation cast the pointer *bits* to `long long`; that
 * bit-cast behaviour now lives in MinHeapComparePtrAsInteger().
 */
PUBLIC_API int MinHeapCompareLongLongKeys(const void *key1, const void *key2);

/**
 * @brief Compare two keys by their pointer bits (as unsigned integers).
 *
 * Use this when the key itself is embedded in the pointer value, e.g. a
 * timestamp stored as `(void *)(intptr_t)time`.
 */
PUBLIC_API int MinHeapComparePtrAsInteger(const void *key1, const void *key2);

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/**
 * @brief Allocate and initialise a pointer-key heap.
 *
 * @param initial_capacity  Entries to reserve up front; 0 chooses a default.
 *                          Negative is rejected (returns NULL).
 * @param comp_func         Key comparator; NULL selects MinHeapCompareIntKeys
 *                          (keys are then `int *`).
 * @return Heap handle, or NULL on allocation failure / invalid capacity.
 *
 * @code{.c}
 * MinHeap *h = MinHeapCreate(64, MinHeapCompareIntKeys);
 * if (!h) return;   // allocation failure
 * MinHeapDestroy(h);
 * @endcode
 */
PUBLIC_API MinHeap *MinHeapCreate(int initial_capacity, MinHeapCmpFn comp_func);

/**
 * @brief Allocate a pointer-key heap that reports through @p logger_ptr.
 *
 * Behaves exactly as MinHeapCreate(), and additionally reports creation, each
 * rejection and allocation failure, and release.  Insert and delmin are never
 * logged, so a heap built this way behaves identically under load to one built
 * without a logger.
 *
 * The failures are worth distinguishing: this constructor can reject a negative
 * capacity, fail on the handle allocation, and fail on the entry array — all of
 * which used to reach the caller as the same bare NULL.
 *
 * The logger is **borrowed, not owned**.  The heap stores the pointer and never
 * destroys it, so the logger must outlive the heap: destroy the heap before
 * destroying the logger.  A heap destroyed after its logger would report
 * through a dangling handle.
 *
 * @param initial_capacity  Entries to reserve up front; 0 chooses a default.
 *                          Negative is rejected (returns NULL).
 * @param comp_func         Key comparator; NULL selects MinHeapCompareIntKeys
 *                          (keys are then `int *`).
 * @param logger_ptr        Logger to report through, or NULL to report nothing.
 *                          NULL gives exactly MinHeapCreate().
 * @return Heap handle, or NULL on allocation failure / invalid capacity.
 *
 * @code{.c}
 * UfLogger *log_ptr = NULL;
 * if (UfLoggerCreateWithDefaults(&log_ptr) != UF_LOGGER_STATUS_OK) { return NULL; }
 *
 * MinHeap *h = MinHeapCreateWithLogger(64, MinHeapCompareIntKeys, log_ptr);
 * if (!h) { UfLoggerDestroy(log_ptr); return NULL; }
 *
 * MinHeapDestroy(h);        // the heap first —
 * UfLoggerDestroy(log_ptr); // then the logger it borrowed
 * @endcode
 */
PUBLIC_API MinHeap *MinHeapCreateWithLogger(int initial_capacity, MinHeapCmpFn comp_func,
                                            UfLogger *logger_ptr);

/**
 * @brief Allocate a heap whose keys are inline `int64_t` values.
 *
 * Comparisons are a direct integer subtract-sign — no comparator function
 * pointer on the insert/delmin hot path.  Use the `_i64` entry points with
 * this handle.
 *
 * @param initial_capacity  Entries to reserve up front; 0 chooses a default.
 * @return Heap handle, or NULL on allocation failure / invalid capacity.
 */
PUBLIC_API MinHeap *MinHeapCreateI64(int initial_capacity);

/**
 * @brief Allocate an inline-`int64_t`-key heap that reports through @p logger_ptr.
 *
 * Behaves exactly as MinHeapCreateI64(), and additionally reports creation and
 * release through the supplied logger.  The borrow contract is the same as
 * MinHeapCreateWithLogger(): the logger is borrowed, never owned, and must
 * outlive the heap.
 *
 * @param initial_capacity  Entries to reserve up front; 0 chooses a default.
 * @param logger_ptr        Logger to report through, or NULL to report nothing.
 *                          NULL gives exactly MinHeapCreateI64().
 * @return Heap handle, or NULL on allocation failure / invalid capacity.
 *
 * @code{.c}
 * MinHeap *h = MinHeapCreateI64WithLogger(32, log_ptr);
 * @endcode
 */
PUBLIC_API MinHeap *MinHeapCreateI64WithLogger(int initial_capacity, UfLogger *logger_ptr);

/**
 * @brief Release backing storage and the handle.
 *
 * Does NOT free pointed-to keys/values (they are caller-owned).  Safe on
 * NULL.  The pointer is invalid afterwards.
 *
 * @param h  Heap handle (NULL → no-op).
 */
PUBLIC_API void MinHeapDestroy(MinHeap *h);

/* ── Introspection ─────────────────────────────────────────────────────── */

/**
 * @brief Number of live entries.
 *
 * @param h  Heap handle (NULL → 0).
 * @return Entry count.
 */
PUBLIC_API int MinHeapSize(const MinHeap *h);

/**
 * @brief Current backing-array capacity in entries.
 *
 * @param h  Heap handle (NULL → 0).
 * @return Capacity (>= size).
 */
PUBLIC_API int MinHeapCapacity(const MinHeap *h);

/**
 * @brief Operating-system page size in bytes.
 *
 * Retained for source compatibility only — storage is now a flat `realloc`
 * buffer, not page-mapped.  The value is computed on each call (glibc
 * caches `sysconf`) and is therefore race-free.
 */
PUBLIC_API int MinHeapPageSize(void);

/**
 * @brief How many internal `MinHeapEntry` records fit in one page.
 *
 * Retained for source compatibility only; see MinHeapPageSize().
 */
PUBLIC_API int MinHeapEntriesPerPage(void);

/* ── Capacity ──────────────────────────────────────────────────────────── */

/**
 * @brief Reserve room for at least `n` live entries.
 *
 * Grows geometrically; never shrinks.  Returns MINHEAP_ERR_OOM for a request
 * that cannot be satisfied (including requests beyond the internal capacity
 * ceiling) rather than overflowing or hanging.
 *
 * @param h  Heap handle.
 * @param n  Minimum capacity wanted (negative → MINHEAP_ERR_INVAL).
 * @return MINHEAP_OK or MINHEAP_ERR_*.
 */
PUBLIC_API int MinHeapReserve(MinHeap *h, int n);

/**
 * @brief Drop all entries; keep the allocation for reuse.
 *
 * The key mode (pointer vs i64) is preserved.  Safe on NULL.
 *
 * @param h  Heap handle.
 */
PUBLIC_API void MinHeapClear(MinHeap *h);

/* ── Insert ────────────────────────────────────────────────────────────── */

/**
 * @brief Insert a pointer key/value.
 *
 * @param h      Pointer-key heap.
 * @param key    Key pointer (comparator reads it; not copied).
 * @param value  Value pointer (caller-owned).
 * @return MINHEAP_OK, MINHEAP_ERR_OOM, or MINHEAP_ERR_INVAL (NULL handle or an i64 heap).
 */
PUBLIC_API int MinHeapInsert(MinHeap *h, void *key, void *value);

/**
 * @brief Insert an inline `int64_t` key.
 *
 * @param h      i64 heap.
 * @param key    Integer key (stored by value).
 * @param value  Value pointer (caller-owned).
 * @return MINHEAP_OK, MINHEAP_ERR_OOM, or MINHEAP_ERR_INVAL (NULL handle or a pointer heap).
 */
PUBLIC_API int MinHeapInsertI64(MinHeap *h, int64_t key, void *value);

/* ── Peek ──────────────────────────────────────────────────────────────── */

/**
 * @brief Peek at the minimum without removing it.
 *
 * @param h      Pointer-key heap.
 * @param key    Out: minimum key (may be NULL).
 * @param value  Out: minimum value (may be NULL).
 * @return 1 if a minimum exists, 0 if empty / NULL / wrong mode.
 */
PUBLIC_API int MinHeapMin(const MinHeap *h, void **key, void **value);

/**
 * @brief Alias of MinHeapMin() (reads the minimum without removing it).
 */
PUBLIC_API int MinHeapPeek(const MinHeap *h, void **key, void **value);

/**
 * @brief Peek at the minimum key of an i64 heap.
 *
 * @param h      i64 heap.
 * @param key    Out: minimum int64 key (may be NULL).
 * @param value  Out: minimum value (may be NULL).
 * @return 1 if a minimum exists, 0 if empty / NULL / wrong mode.
 */
PUBLIC_API int MinHeapMinI64(const MinHeap *h, int64_t *key, void **value);

/* ── Remove ────────────────────────────────────────────────────────────── */

/**
 * @brief Remove and return the minimum (pointer-key heap).
 *
 * @param h      Pointer-key heap.
 * @param key    Out: removed key (may be NULL).
 * @param value  Out: removed value (may be NULL).
 * @return 1 if an element was removed, 0 if empty / NULL / wrong mode.
 */
PUBLIC_API int MinHeapDelmin(MinHeap *h, void **key, void **value);

/**
 * @brief Remove and return the minimum (i64 heap).
 *
 * @param h      i64 heap.
 * @param key    Out: removed int64 key (may be NULL).
 * @param value  Out: removed value (may be NULL).
 * @return 1 if an element was removed, 0 if empty / NULL / wrong mode.
 */
PUBLIC_API int MinHeapDelminI64(MinHeap *h, int64_t *key, void **value);

/**
 * @brief Remove the minimum and return only its value (either key mode).
 *
 * @param h      Heap handle (either mode).
 * @param value  Out: removed value (may be NULL).
 * @return 1 if an element was removed, 0 otherwise.
 */
PUBLIC_API int MinHeapPop(MinHeap *h, void **value);

/* ── Iterate ───────────────────────────────────────────────────────────── */

/**
 * @brief Visit every live entry.
 *
 * Order is the heap's backing-array order, which is *not* sorted order.  The
 * callback must not mutate the heap.
 *
 * For a pointer-key heap the callback's `key` is the stored key pointer.  For
 * an i64 heap the callback's `key` is the `int64_t` key value re-interpreted
 * as `void *`; cast it back with `(intptr_t)key` to recover the integer.
 *
 * @param h         Heap handle.
 * @param func      Visitor callback (key, value, ctx).
 * @param ctx_data  Opaque context forwarded to the callback.
 * @return Number of entries visited, or 0 for NULL/empty.
 */
PUBLIC_API size_t MinHeapForeach(const MinHeap *h,
                               void (*func)(void *key, void *value, void *ctx),
                               ClientContextData *ctx_data);

/* ── Introspection (JSON) ──────────────────────────────────────────────── */

/**
 * @brief Describe the heap's state as a JSON string.
 *
 * Composes a best-effort introspection snapshot: live size, backing
 * capacity, key mode (`"ptr"` or `"i64"`), load factor, whether a comparator
 * is installed, whether this heap was created with a logger to report through
 * (`logger`, `"enabled"` or `"none"`), and one entry per live slot in
 * heap-array order (index, key, value).  For a pointer-key heap the key/value
 * are rendered as hex addresses (`null` when NULL); for an i64 heap the key is
 * rendered as a decimal integer.
 *
 * The heap is single-threaded, so the snapshot is exact (unlike the lock-free
 * introspection of DescribeLocklessLru(), which is approximate under
 * concurrency).
 *
 * If @p provided is NULL, a fresh BufferDescriptor is allocated (the caller
 * must free it with BufferDescriptorRelease() + free()); otherwise the
 * caller's descriptor is appended to.  Returns @p provided, or NULL only if a
 * fresh descriptor could not be allocated.
 *
 * @param h         Heap handle (NULL → emits `{"error":"null handle"}`, which
 *                  carries no `logger` attribute: with no instance there is no
 *                  borrow to report).
 * @param provided  Optional caller-owned BufferDescriptor (NULL → allocate).
 * @return The populated BufferDescriptor.
 *
 * @code{.c}
 * BufferDescriptor bd;
 * BufferDescriptorInit(&bd, 512);
 * MinHeapDescribe(h, &bd);
 * printf("%s\n", bd.data);
 * BufferDescriptorRelease(&bd);
 * @endcode
 */
PUBLIC_API BufferDescriptor *
MinHeapDescribe(const MinHeap *h, BufferDescriptor *provided);

#ifdef __cplusplus
}
#endif

#endif /* UFLIB_ADT_ADT_MINHEAP_H */
