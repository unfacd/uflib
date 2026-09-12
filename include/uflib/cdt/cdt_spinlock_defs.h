/**
 * @file cdt_spinlock_defs.h
 * @brief Shared definitions for CDT spinlock modules — C11/C++17 atomics
 *        bridge, SPINWAIT() platform abstraction, and cache-line constant.
 *
 * Include this header once before defining spinlock types or functions.
 * It provides a single, consistent C++ bridge so that spinlock headers
 * compile natively under both C11 (production) and C++17 (gtest harness).
 *
 * ## What this provides
 *
 * | Symbol                      | C11 (production)              | C++17 (gtest)                        |
 * |-----------------------------|-------------------------------|--------------------------------------|
 * | `_Atomic(T)`                | Native C11 keyword            | `std::atomic<T>`                     |
 * | `UFLIB_MO_ACQUIRE`          | `memory_order_acquire`        | `std::memory_order_acquire`          |
 * | `UFLIB_MO_RELEASE`          | `memory_order_release`        | `std::memory_order_release`          |
 * | `UFLIB_MO_RELAXED`          | `memory_order_relaxed`        | `std::memory_order_relaxed`          |
 * | `UFLIB_MO_ACQ_REL`          | `memory_order_acq_rel`        | `std::memory_order_acq_rel`          |
 * | `atomic_init(OBJ, VAL)`     | `atomic_init(&f, v)`          | Relaxed `.store(v)`                  |
 * | `atomic_load_explicit`      | Native C11 function           | `.load(mo)` member function          |
 * | `atomic_store_explicit`     | Native C11 function           | `.store(v, mo)` member function      |
 * | `atomic_exchange_explicit`  | Native C11 function           | `.exchange(v, mo)` member function   |
 * | `atomic_fetch_add_explicit` | Native C11 function           | `.fetch_add(v, mo)` member function  |
 * | `atomic_compare_exchange_strong_explicit` | Native C11 function | `.compare_exchange_strong(...)` |
 * | `atomic_thread_fence`       | Native C11 function           | `std::atomic_thread_fence(mo)`       |
 * | `SPINWAIT()`                | `pause` (x86) / `yield` (ARM) / no-op | same                       |
 * | `CDT_CACHELINE_SZ`          | 64                            | 64                                   |
 *
 * @see cdt_anderson_spinlock.h
 * @see cdt_mcs_lock.h
 */

/*
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

#ifndef UFLIB_CDT_SPINLOCK_DEFS_H
#define UFLIB_CDT_SPINLOCK_DEFS_H

#include <stddef.h>

/* ── C11 / C++17 atomics bridge ──────────────────────────────────────────── */

/*
 * Use _Atomic(T) in struct definitions so the existing C++ bridge in
 * standard_c_includes.h (#define _Atomic(X) std::atomic<X>) converts
 * seamlessly.  For the memory_order constants, map C11 identifiers into
 * the std:: namespace when compiling as C++.
 *
 * atomic_init is mapped to a relaxed store in C++ mode because
 * std::atomic_init() was deprecated in C++17 and removed in C++20.
 * The relaxed store is safe here because init always precedes a
 * release fence or an exchange that provides the happens-before edge.
 */
#ifdef __cplusplus
#include <atomic>
#include <climits>
#ifndef _Atomic
#define _Atomic(T) std::atomic<T>
#endif
#define UFLIB_MO_ACQUIRE std::memory_order_acquire
#define UFLIB_MO_RELEASE std::memory_order_release
#define UFLIB_MO_RELAXED std::memory_order_relaxed
#define UFLIB_MO_ACQ_REL std::memory_order_acq_rel
#define atomic_init(OBJ, VAL) \
    atomic_store_explicit((OBJ), (VAL), UFLIB_MO_RELAXED)
#define atomic_load_explicit(OBJ, MO)    ((OBJ)->load((MO)))
#define atomic_store_explicit(OBJ, VAL, MO) ((OBJ)->store((VAL), (MO)))
#define atomic_exchange_explicit(OBJ, VAL, MO) \
    ((OBJ)->exchange((VAL), (MO)))
#define atomic_fetch_add_explicit(OBJ, VAL, MO) \
    ((OBJ)->fetch_add((VAL), (MO)))
#define atomic_compare_exchange_strong_explicit(OBJ, EXPECTED, DESIRED, \
                                                 SUCCESS, FAIL) \
    ((OBJ)->compare_exchange_strong(*(EXPECTED), (DESIRED), \
                                     (SUCCESS), (FAIL)))
#define atomic_thread_fence(MO) std::atomic_thread_fence((MO))
#else
#include <stdatomic.h>
#include <stdbool.h>
#include <limits.h>
#define UFLIB_MO_ACQUIRE memory_order_acquire
#define UFLIB_MO_RELEASE memory_order_release
#define UFLIB_MO_RELAXED memory_order_relaxed
#define UFLIB_MO_ACQ_REL memory_order_acq_rel
#endif

/* ── Spinwait platform abstraction ───────────────────────────────────────── */

#ifndef SPINWAIT

#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__amd64__) || defined(__x86_64__) || \
     defined(__i386__) || defined(__ia64__))
#define SPINWAIT() do { __asm __volatile("pause" ::: "memory"); } while (0)

#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#define SPINWAIT() do { __asm { __asm pause }; } while (0)

#else
/* No pause instruction on other platforms/compilers. */
#define SPINWAIT() do { /* nothing */ } while (0)

#endif /* SPINWAIT platform selector. */

#endif /* SPINWAIT */

/* ── Constants ───────────────────────────────────────────────────────────── */

/*! Cache-line size for padding (x86_64 / ARM64). */
#define CDT_CACHELINE_SZ 64

#endif /* UFLIB_CDT_SPINLOCK_DEFS_H */
