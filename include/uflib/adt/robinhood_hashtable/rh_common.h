/**
 * @file rh_common.h
 * @brief Shared helpers for rh_template.h: hashing, allocation and result codes.
 *
 * The table template is included once per key and value type, so everything
 * that is not parameterised — the mixing functions, the aligned allocator and
 * the result codes — lives here and is instantiated once per translation unit.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_RH_COMMON_H
#define UFLIB_RH_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
# define RH_TEMPLATE_HOT static inline __attribute__((always_inline))
# define RH_TEMPLATE_PREFETCH(address) __builtin_prefetch((address), 0, 3)
# define RH_TEMPLATE_UNLIKELY(expression) __builtin_expect(!!(expression), 0)
#else
# define RH_TEMPLATE_HOT static inline
# define RH_TEMPLATE_PREFETCH(address) ((void)(address))
# define RH_TEMPLATE_UNLIKELY(expression) (expression)
#endif

/** Outcome of an instantiated table operation. The values are the contract. */
typedef enum RhResult {
  RH_TEMPLATE_FULL = -2,     ///< a bounded table is at its entry limit
  RH_TEMPLATE_ERROR = -1,    ///< allocation failure, or the reserved empty key
  RH_TEMPLATE_INSERTED = 0,  ///< the key was absent and is now stored
  RH_TEMPLATE_UPDATED = 1    ///< the key was present and its value was replaced
} RhResult;

#define RhtCat_(left, right) left##right
#define RhtCat(left, right) RhtCat_(left, right)
#define RH_TEMPLATE_CACHELINE 64u
#define RH_TEMPLATE_MIN_SLOTS 16u
#define RH_TEMPLATE_BATCH 16u

static inline uint64_t
sRhtSplitMix64(uint64_t state)
{
  state += 0x9E3779B97F4A7C15ull;
  state = (state ^ (state >> 30)) * 0xBF58476D1CE4E5B9ull;
  state = (state ^ (state >> 27)) * 0x94D049BB133111EBull;
  return state ^ (state >> 31);
}

static inline uint64_t
sRhtFoldMultiply(uint64_t left, uint64_t right)
{
#ifdef __SIZEOF_INT128__
  __uint128_t product = (__uint128_t)left * right;
  return (uint64_t)product ^ (uint64_t)(product >> 64);
#else
  uint64_t mixed = left * 0x9E3779B97F4A7C15ull;
  mixed ^= mixed >> 32;
  mixed += right * 0xD6E8FEB86659FD93ull;
  mixed ^= mixed >> 29;
  mixed *= 0xBF58476D1CE4E5B9ull;
  return mixed ^ (mixed >> 32);
#endif
}

static inline uint64_t
sRhtLoad64(const void *bytes)
{
  uint64_t value;
  memcpy(&value, bytes, sizeof(value));
  return value;
}

/* Zeroed, cache-line aligned, overflow-checked. NULL on failure. */
static inline void *
sRhtAllocZeroed(size_t count, size_t element_size)
{
  if (element_size && count > SIZE_MAX / element_size) return NULL;
  size_t bytes = count * element_size;
  if (bytes > SIZE_MAX - (RH_TEMPLATE_CACHELINE - 1)) return NULL;
  bytes = (bytes + RH_TEMPLATE_CACHELINE - 1) & ~(size_t)(RH_TEMPLATE_CACHELINE - 1);
  void *storage = aligned_alloc(RH_TEMPLATE_CACHELINE, bytes);
  if (storage) memset(storage, 0, bytes);
  return storage;
}

/* Power-of-two slot count holding @p entries at most 7/8 full; 0 on overflow. */
static inline size_t
sRhtSlotsFor(size_t entries)
{
  size_t needed = entries + entries / 7 + 1;
  if (needed < entries) return 0;
  size_t slots = RH_TEMPLATE_MIN_SLOTS;
  while (slots < needed) {
    if (slots > SIZE_MAX / 2) return 0;
    slots <<= 1;
  }
  return slots;
}

#endif /* UFLIB_RH_COMMON_H */
