/**
 * @file rh_template.h
 * @brief Robin Hood flat hash table, instantiated once per key and value type.
 *
 * Include this header (any number of times) after defining:
 *
 *   RhtTemplateName          the PascalCase base name; generates s<Name>Create, ...
 *   RhtTemplateKey           key type, stored inline in the slot
 *   RhtTemplateValue         value type, stored inline (a u32 index or a pointer)
 *   RhtTemplateHash(k, a, b) k is const RhtTemplateKey *; a and b are the per-table
 *                            secrets. Xor both into your multiplies.
 *   RhtTemplateKeyEq(l, r)   both are const RhtTemplateKey *
 *   RhtTemplateSlotEmpty(s)  s is a const slot *; true for an empty slot.
 *                            Whatever looks empty cannot be stored: insertion
 *                            returns RH_TEMPLATE_ERROR for it. Pick the cheapest
 *                            sentinel (value == NULL, ports == 0, ...), so a zero
 *                            key stays valid where the value carries the sentinel.
 *
 * Slots are plain structs of the key followed by the value: a lookup walks the
 * bucket array and follows no pointer. Slot addresses are NOT stable across
 * mutation; hold values, never slot pointers. Not thread-safe for writers.
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

/* No include guard: one translation unit instantiates this header per table. */

#include <uflib/adt/robinhood_hashtable/rh_common.h>

#if !defined(RhtTemplateName) || !defined(RhtTemplateKey) || !defined(RhtTemplateValue) || \
    !defined(RhtTemplateHash) || !defined(RhtTemplateKeyEq) || !defined(RhtTemplateSlotEmpty)
# error "rh_template.h: define RhtTemplateName, RhtTemplateKey, RhtTemplateValue, RhtTemplateHash, RhtTemplateKeyEq, RhtTemplateSlotEmpty"
#endif

#define RhtFn(suffix) RhtCat(RhtCat(s, RhtTemplateName), suffix)
#define RhtSlot RhtCat(RhtTemplateName, Slot)
#define RhtTable struct RhtTemplateName

/** One table entry: the key inline, then its value. Not addressable across mutation. */
typedef struct RhtSlot {
  RhtTemplateKey   key;
  RhtTemplateValue value;
} RhtSlot;

typedef struct RhtTemplateName RhtTemplateName;
struct RhtTemplateName {
  RhtSlot *slots;
  size_t   mask;        ///< slot count - 1
  size_t   count;
  size_t   grow_at;     ///< slot count - slot count / 8
  size_t   max_slots;   ///< 0 = unbounded
  size_t   max_entries; ///< 0 = unbounded; otherwise the exact entry limit
  size_t   slow_at;     ///< min(grow_at, max_entries): one compare on the insert fast path
  uint64_t secret_a;    ///< per-table hash secrets; never zero
  uint64_t secret_b;
};

RH_TEMPLATE_HOT RhtSlot *
RhtFn(Lookup)(const RhtTable *table, const RhtTemplateKey *key, uint64_t hash)
{
  RhtSlot *slots = table->slots;
  const size_t mask = table->mask;
  size_t index = (size_t)hash & mask;
  for (size_t distance = 0;; distance++) {
    RhtSlot *slot = &slots[index];
    if (RhtTemplateSlotEmpty(slot)) return NULL;
    if (RhtTemplateKeyEq(&slot->key, key)) return slot;
    if (((index - (size_t)RhtTemplateHash(&slot->key, table->secret_a, table->secret_b)) & mask) <
        distance) {
      return NULL;
    }
    index = (index + 1) & mask;
  }
}

/* Place @p current into the table. allow_update is false while rehashing, where
 * the keys are known to differ. */
RH_TEMPLATE_HOT RhResult
RhtFn(Put)(const RhtTable *table, RhtSlot *slots, size_t mask, RhtSlot current, uint64_t hash,
           bool allow_update)
{
  size_t index = (size_t)hash & mask, distance = 0;
  bool displaced = false; /* an existing key can only match before the first swap */
  for (;;) {
    RhtSlot *slot = &slots[index];
    if (RhtTemplateSlotEmpty(slot)) {
      *slot = current;
      return RH_TEMPLATE_INSERTED;
    }
    if (!displaced && allow_update && RhtTemplateKeyEq(&slot->key, &current.key)) {
      slot->value = current.value;
      return RH_TEMPLATE_UPDATED;
    }
    size_t resident = (index - (size_t)RhtTemplateHash(&slot->key, table->secret_a, table->secret_b)) & mask;
    if (resident < distance) {
      RhtSlot displaced_slot = *slot;
      *slot = current;
      current = displaced_slot;
      distance = resident;
      displaced = true;
    }
    index = (index + 1) & mask;
    distance++;
  }
}

static inline int
RhtFn(Grow)(RhtTable *table)
{
  const size_t slots = table->mask + 1;
  if (table->max_slots && slots >= table->max_slots) return RH_TEMPLATE_FULL;
  if (slots > SIZE_MAX / 2) return RH_TEMPLATE_ERROR;
  const size_t grown_slots = slots * 2;
  RhtSlot *grown = (RhtSlot *)sRhtAllocZeroed(grown_slots, sizeof(RhtSlot));
  if (!grown) return RH_TEMPLATE_ERROR;
  for (size_t index = 0; index < slots; index++) {
    if (RhtTemplateSlotEmpty(&table->slots[index])) continue;
    (void)RhtFn(Put)(table, grown, grown_slots - 1, table->slots[index],
                     RhtTemplateHash(&table->slots[index].key, table->secret_a, table->secret_b), false);
  }
  free(table->slots);
  table->slots = grown;
  table->mask = grown_slots - 1;
  table->grow_at = grown_slots - (grown_slots >> 3);
  table->slow_at = table->max_entries && table->max_entries < table->grow_at ? table->max_entries
                                                                            : table->grow_at;
  return 0;
}

/**
 * @brief Initialise a caller-owned table header.
 *
 * For a table embedded in another structure (one allocation, one indirection
 * saved on every probe). @p table is zeroed first, so a failed init can be
 * followed by Fini safely.
 *
 * @param initial_entries Expected entry count; 0 picks the minimum capacity.
 * @param max_entries     Hard limit on entries, or 0 for unbounded. A bounded
 *                        table never reallocates once at that size and returns
 *                        RH_TEMPLATE_FULL beyond it.
 * @param seed            Hash seed; randomise it when keys are client chosen.
 *
 * @return 0 on success, negative on overflow or allocation failure.
 */
static inline int
RhtFn(Init)(RhtTable *table, size_t initial_entries, size_t max_entries, uint64_t seed)
{
  memset(table, 0, sizeof(*table));
  size_t max_slots = 0;
  if (max_entries) {
    max_slots = sRhtSlotsFor(max_entries);
    if (!max_slots) return RH_TEMPLATE_ERROR;
  }
  size_t slots = sRhtSlotsFor(initial_entries);
  if (!slots) return RH_TEMPLATE_ERROR;
  if (max_slots && slots > max_slots) slots = max_slots;

  RhtSlot *storage = (RhtSlot *)sRhtAllocZeroed(slots, sizeof(RhtSlot));
  if (!storage) return RH_TEMPLATE_ERROR;
  table->slots = storage;
  table->mask = slots - 1;
  table->count = 0;
  table->grow_at = slots - (slots >> 3);
  table->max_slots = max_slots;
  table->max_entries = max_entries;
  table->slow_at = max_entries && max_entries < table->grow_at ? max_entries : table->grow_at;
  table->secret_a = sRhtSplitMix64(seed);
  table->secret_b = sRhtSplitMix64(table->secret_a ^ seed ^ 0xD6E8FEB86659FD93ull);
  if (!table->secret_a) table->secret_a = 0x9E3779B97F4A7C15ull;
  if (!table->secret_b) table->secret_b = 0xBF58476D1CE4E5B9ull;
  return 0;
}

/** Release the slot array of an initialised table; the header is zeroed after,
 * so a second call is a no-op. The header itself is not freed. */
static inline void
RhtFn(Fini)(RhtTable *table)
{
  free(table->slots);
  memset(table, 0, sizeof(*table));
}

/**
 * @brief Create a heap table.
 *
 * Same sizing contract as Init; the header and slots are released by Destroy.
 */
static inline RhtTable *
RhtFn(Create)(size_t initial_entries, size_t max_entries, uint64_t seed)
{
  RhtTable *table = (RhtTable *)malloc(sizeof(*table));
  if (!table) return NULL;
  if (RhtFn(Init)(table, initial_entries, max_entries, seed) != 0) {
    free(table);
    return NULL;
  }
  return table;
}

static inline void
RhtFn(Destroy)(RhtTable *table)
{
  if (!table) return;
  RhtFn(Fini)(table);
  free(table);
}

static inline void
RhtFn(Clear)(RhtTable *table)
{
  memset(table->slots, 0, (table->mask + 1) * sizeof(RhtSlot));
  table->count = 0;
}

static inline RhResult
RhtFn(Insert)(RhtTable *table, const RhtTemplateKey *key, RhtTemplateValue value)
{
  RhtSlot current = { .key = *key, .value = value };
  if (RhtTemplateSlotEmpty(&current)) return RH_TEMPLATE_ERROR;
  const uint64_t hash = RhtTemplateHash(key, table->secret_a, table->secret_b);
  if (RH_TEMPLATE_UNLIKELY(table->count >= table->slow_at)) {
    RhtSlot *existing = RhtFn(Lookup)(table, key, hash); /* an update never needs room */
    if (existing) {
      existing->value = value;
      return RH_TEMPLATE_UPDATED;
    }
    if (table->max_entries && table->count >= table->max_entries) return RH_TEMPLATE_FULL;
    if (table->count >= table->grow_at) {
      int grown = RhtFn(Grow)(table);
      if (grown) return (RhResult)grown;
    }
  }
  RhResult result = RhtFn(Put)(table, table->slots, table->mask, current, hash, true);
  if (result == RH_TEMPLATE_INSERTED) table->count++;
  return result;
}

static inline bool
RhtFn(Find)(const RhtTable *table, const RhtTemplateKey *key, RhtTemplateValue *value)
{
  RhtSlot *slot = RhtFn(Lookup)(table, key, RhtTemplateHash(key, table->secret_a, table->secret_b));
  if (!slot) return false;
  if (value) *value = slot->value;
  return true;
}

/* Value pointer for in-place update; valid only until the next mutation. */
static inline RhtTemplateValue *
RhtFn(Get)(RhtTable *table, const RhtTemplateKey *key)
{
  RhtSlot *slot = RhtFn(Lookup)(table, key, RhtTemplateHash(key, table->secret_a, table->secret_b));
  return slot ? &slot->value : NULL;
}

static inline bool
RhtFn(Erase)(RhtTable *table, const RhtTemplateKey *key)
{
  RhtSlot *found = RhtFn(Lookup)(table, key, RhtTemplateHash(key, table->secret_a, table->secret_b));
  if (!found) return false;
  RhtSlot *slots = table->slots;
  const size_t mask = table->mask;
  size_t index = (size_t)(found - slots);
  for (;;) { /* backward-shift deletion keeps the probe chains intact */
    size_t next = (index + 1) & mask;
    if (RhtTemplateSlotEmpty(&slots[next]) ||
        ((next - (size_t)RhtTemplateHash(&slots[next].key, table->secret_a, table->secret_b)) & mask) == 0) {
      break;
    }
    slots[index] = slots[next];
    index = next;
  }
  memset(&slots[index], 0, sizeof(RhtSlot));
  table->count--;
  return true;
}

/**
 * @brief Resolve a batch of keys: hash and prefetch first, probe second.
 *
 * @param values  Receives the value per hit; entries for misses are untouched.
 * @param found   Receives the per-key outcome.
 *
 * @return The number of hits.
 */
static inline size_t
RhtFn(FindBatch)(const RhtTable *table, const RhtTemplateKey *keys, size_t count,
                 RhtTemplateValue *values, bool *found)
{
  size_t hits = 0;
  uint64_t hashes[RH_TEMPLATE_BATCH];
  for (size_t base = 0; base < count; base += RH_TEMPLATE_BATCH) {
    size_t batch = count - base < RH_TEMPLATE_BATCH ? count - base : RH_TEMPLATE_BATCH;
    for (size_t index = 0; index < batch; index++) {
      hashes[index] = RhtTemplateHash(&keys[base + index], table->secret_a, table->secret_b);
      const RhtSlot *slot = &table->slots[(size_t)hashes[index] & table->mask];
      RH_TEMPLATE_PREFETCH(slot);
      RH_TEMPLATE_PREFETCH((const char *)slot + sizeof(RhtSlot) - 1); /* straddling slot */
    }
    for (size_t index = 0; index < batch; index++) {
      RhtSlot *slot = RhtFn(Lookup)(table, &keys[base + index], hashes[index]);
      found[base + index] = slot != NULL;
      if (slot) {
        values[base + index] = slot->value;
        hits++;
      }
    }
  }
  return hits;
}

/* Iterate: start with *cursor = 0. No insert or erase while iterating. */
static inline bool
RhtFn(Next)(const RhtTable *table, size_t *cursor, const RhtTemplateKey **key, RhtTemplateValue *value)
{
  for (; *cursor <= table->mask; (*cursor)++) {
    const RhtSlot *slot = &table->slots[*cursor];
    if (RhtTemplateSlotEmpty(slot)) continue;
    *key = &slot->key;
    *value = slot->value;
    (*cursor)++;
    return true;
  }
  return false;
}

static inline size_t
RhtFn(Size)(const RhtTable *table)
{
  return table->count;
}

static inline size_t
RhtFn(Capacity)(const RhtTable *table)
{
  return table->mask + 1;
}

#undef RhtTemplateName
#undef RhtTemplateKey
#undef RhtTemplateValue
#undef RhtTemplateHash
#undef RhtTemplateKeyEq
#undef RhtTemplateSlotEmpty
#undef RhtFn
#undef RhtSlot
#undef RhtTable
