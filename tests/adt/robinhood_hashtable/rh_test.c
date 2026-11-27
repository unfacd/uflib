/**
 * @file rh_test.c
 * @brief Exercises rh_template.h through two instantiation shapes in one unit.
 *
 * The first instantiation is the identifier-map shape (u64 to pointer, key zero
 * valid); the second is a scalar (u32 to u32) whose value zero is storable. Two
 * instantiations in one translation unit is the property the table modules rely
 * on: the header is included once per type, never guarded. That shape runs
 * 3M random operations against a reference model, then the bounded, reserved-key
 * and overflow-safe creation contracts are pinned.
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

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <uflib/adt/robinhood_hashtable/rh_common.h>

static uint64_t sRandomState = 88172645463325252ull;

static uint32_t
sRandom(void)
{
  sRandomState ^= sRandomState << 13;
  sRandomState ^= sRandomState >> 7;
  sRandomState ^= sRandomState << 17;
  return (uint32_t)(sRandomState >> 16);
}

/* --- instantiation one: u64 -> pointer, key zero valid (the identifier map) --- */
#define RhtTemplateName U64Map
#define RhtTemplateKey uint64_t
#define RhtTemplateValue void *
#define RhtTemplateHash(identifier, secret_a, secret_b) sRhtFoldMultiply(*(identifier) ^ (secret_a), (secret_b))
#define RhtTemplateKeyEq(left, right) (*(left) == *(right))
#define RhtTemplateSlotEmpty(slot) ((slot)->value == NULL)
#include <uflib/adt/robinhood_hashtable/rh_template.h>

/* --- instantiation two: scalar, with zero a storable value --- */
#define RhtTemplateName U32Map
#define RhtTemplateKey uint32_t
#define RhtTemplateValue uint32_t
#define RhtTemplateHash(key, secret_a, secret_b) sRhtFoldMultiply((uint64_t)*(key) ^ (secret_a), (secret_b))
#define RhtTemplateKeyEq(left, right) (*(left) == *(right))
#define RhtTemplateSlotEmpty(slot) ((slot)->key == 0 && (slot)->value == 0)
#include <uflib/adt/robinhood_hashtable/rh_template.h>

_Static_assert(sizeof(U64MapSlot) == 16, "the identifier slot is the id plus a pointer");
_Static_assert(sizeof(U32MapSlot) == 8, "the scalar slot is a key and a value");

static void *
sValueFor(uint64_t identifier)
{
  return (void *)(uintptr_t)(identifier + 1);
}

int
main(void)
{
  /* identifier map against a reference model, including key zero and erase churn */
  enum { MODEL = 200000 };
  static void *model[MODEL];
  U64Map *entries = sU64MapCreate(0, 0, 42);
  size_t present = 0;
  for (int round = 0; round < 3000000; round++) {
    uint64_t identifier = sRandom() % MODEL;
    switch (sRandom() % 3) {
    case 0: {
      RhResult result = sU64MapInsert(entries, &identifier, sValueFor(identifier));
      assert(result == (model[identifier] ? RH_TEMPLATE_UPDATED : RH_TEMPLATE_INSERTED));
      if (!model[identifier]) present++;
      model[identifier] = sValueFor(identifier);
      break;
    }
    case 1: {
      bool erased = sU64MapErase(entries, &identifier);
      assert(erased == (model[identifier] != NULL));
      if (model[identifier]) present--;
      model[identifier] = NULL;
      break;
    }
    default: {
      void *value = NULL;
      bool found = sU64MapFind(entries, &identifier, &value);
      assert(found == (model[identifier] != NULL) && (!found || value == model[identifier]));
    }
    }
    assert(sU64MapSize(entries) == present);
  }

  size_t cursor = 0, iterated = 0;
  const uint64_t *iterated_key;
  void *iterated_value;
  while (sU64MapNext(entries, &cursor, &iterated_key, &iterated_value)) {
    assert(model[*iterated_key] == iterated_value);
    iterated++;
  }
  assert(iterated == present);

  uint64_t key_zero = 0;
  assert(sU64MapInsert(entries, &key_zero, NULL) == RH_TEMPLATE_ERROR);

  static uint64_t batch_keys[1000];
  static void *batch_values[1000];
  static bool batch_found[1000];
  for (int index = 0; index < 1000; index++) batch_keys[index] = sRandom() % MODEL;
  size_t hits = sU64MapFindBatch(entries, batch_keys, 1000, batch_values, batch_found), expected_hits = 0;
  for (int index = 0; index < 1000; index++) {
    assert(batch_found[index] == (model[batch_keys[index]] != NULL));
    expected_hits += batch_found[index];
  }
  assert(hits == expected_hits);
  sU64MapDestroy(entries);

  /* bounded table: fills, rejects, still updates, reuses after erase */
  U64Map *bounded = sU64MapCreate(1000, 1000, 7);
  size_t original_capacity = sU64MapCapacity(bounded);
  for (uint64_t identifier = 0; identifier < 1000; identifier++) {
    assert(sU64MapInsert(bounded, &identifier, sValueFor(identifier)) == RH_TEMPLATE_INSERTED);
  }
  uint64_t extra = 5000;
  assert(sU64MapInsert(bounded, &extra, sValueFor(extra)) == RH_TEMPLATE_FULL);
  uint64_t update = 10;
  assert(sU64MapInsert(bounded, &update, sValueFor(99)) == RH_TEMPLATE_UPDATED);
  assert(sU64MapErase(bounded, &update));
  assert(sU64MapInsert(bounded, &extra, sValueFor(extra)) == RH_TEMPLATE_INSERTED);
  assert(sU64MapCapacity(bounded) == original_capacity);
  sU64MapDestroy(bounded);

  /* overflow-safe creation: both of these are rejected before allocating */
  assert(sU64MapCreate(SIZE_MAX / 2, 0, 1) == NULL);
  assert(sU64MapCreate(SIZE_MAX, 0, 1) == NULL);
  assert(sU64MapCreate(1, SIZE_MAX, 1) == NULL);

  /* the second instantiation shares the unit and stores a zero value */
  U32Map *scalars = sU32MapCreate(0, 0, 3);
  uint32_t scalar_key = 7;
  assert(sU32MapInsert(scalars, &scalar_key, 0) == RH_TEMPLATE_INSERTED);
  uint32_t scalar_value = 1;
  assert(sU32MapFind(scalars, &scalar_key, &scalar_value) && scalar_value == 0);
  uint32_t absent_key = 0;
  assert(!sU32MapFind(scalars, &absent_key, NULL));
  sU32MapDestroy(scalars);

  puts("ok");
  return 0;
}
