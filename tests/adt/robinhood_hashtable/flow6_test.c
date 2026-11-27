/**
 * @file flow6_test.c
 * @brief Reference-model churn, batch lookups and bounded behaviour of flow6.
 *
 * The table is checked against a boolean model over 2.5M random operations, then
 * the details the flow table relies on: the two ports as separate fields, every
 * address byte significant, the reserved all-zero-ports key, the exact entry
 * limit of a bounded table and its reuse after erase.
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
#include <string.h>

#include <uflib/adt/robinhood_hashtable/flow6.h>

static uint64_t sRandomState = 88172645463325252ull;

static uint32_t
sRandom(void)
{
  sRandomState ^= sRandomState << 13;
  sRandomState ^= sRandomState >> 7;
  sRandomState ^= sRandomState << 17;
  return (uint32_t)(sRandomState >> 16);
}

/* Injective in @p identifier; the two ports are never both zero. */
static Flow6Key
sMakeKey(uint32_t identifier)
{
  Flow6Key key;
  memset(&key, 0, sizeof key);
  key.source[0] = 0x20;
  key.source[1] = 0x01;
  key.source[2] = 0x0d;
  key.source[3] = 0xb8;
  key.source[12] = (uint8_t)(identifier >> 24);
  key.source[13] = (uint8_t)(identifier >> 16);
  key.source[14] = (uint8_t)(identifier >> 8);
  key.source[15] = (uint8_t)identifier;
  key.destination[0] = 0xfe;
  key.destination[1] = 0x80;
  key.destination[15] = (uint8_t)(identifier % 13 + 1);
  key.source_port = (uint16_t)(1 + identifier % 65000);
  key.destination_port = (identifier % 7) ? 443 : 80;
  return key;
}

int
main(void)
{
  enum { MODEL = 150000 };
  static bool present[MODEL];
  static uint32_t values[MODEL];
  Flow6Table *table = Flow6Create(0, 0, 0x1234abcd);
  size_t count = 0;

  for (int round = 0; round < 2500000; round++) { /* churn against the model */
    uint32_t identifier = sRandom() % MODEL;
    Flow6Key key = sMakeKey(identifier);
    switch (sRandom() % 3) {
    case 0: {
      uint32_t value = sRandom();
      int result = Flow6Insert(table, &key, value);
      assert(result == (present[identifier] ? FLOW6_UPDATED : FLOW6_INSERTED));
      if (!present[identifier]) count++;
      present[identifier] = true;
      values[identifier] = value;
      break;
    }
    case 1: {
      bool erased = Flow6Erase(table, &key);
      assert(erased == present[identifier]);
      if (present[identifier]) count--;
      present[identifier] = false;
      break;
    }
    default: {
      uint32_t value = 0;
      bool found = Flow6Find(table, &key, &value);
      assert(found == present[identifier] && (!found || value == values[identifier]));
    }
    }
    assert(Flow6Size(table) == count);
  }

  static Flow6Key batch_keys[2048];
  static uint32_t batch_values[2048];
  static bool batch_found[2048];
  uint32_t identifiers[2048];
  for (int index = 0; index < 2048; index++) {
    identifiers[index] = sRandom() % MODEL;
    batch_keys[index] = sMakeKey(identifiers[index]);
  }
  size_t hits = Flow6FindBatch(table, batch_keys, 2048, batch_values, batch_found), expected_hits = 0;
  for (int index = 0; index < 2048; index++) {
    assert(batch_found[index] == present[identifiers[index]]);
    if (batch_found[index]) {
      assert(batch_values[index] == values[identifiers[index]]);
      expected_hits++;
    }
  }
  assert(hits == expected_hits);
  Flow6Destroy(table);

  /* the ports are distinct fields; every address byte matters */
  table = Flow6Create(0, 0, 1);
  Flow6Key first = sMakeKey(5), swapped_ports = first;
  swapped_ports.source_port = first.destination_port;
  swapped_ports.destination_port = first.source_port;
  assert(Flow6Insert(table, &first, 1) == FLOW6_INSERTED);
  assert(Flow6Insert(table, &swapped_ports, 2) == FLOW6_INSERTED);
  uint32_t value;
  assert(Flow6Find(table, &first, &value) && value == 1);
  assert(Flow6Find(table, &swapped_ports, &value) && value == 2);
  for (int byte = 0; byte < 32; byte++) {
    Flow6Key mutated = first;
    (byte < 16 ? mutated.source : mutated.destination)[byte & 15] ^= 0x80;
    assert(!Flow6Find(table, &mutated, NULL));
  }

  /* the reserved key: both ports zero */
  Flow6Key reserved = sMakeKey(9);
  reserved.source_port = 0;
  reserved.destination_port = 0;
  assert(Flow6Insert(table, &reserved, 1) == FLOW6_ERROR);
  assert(!Flow6Find(table, &reserved, NULL));
  assert(!Flow6Erase(table, &reserved));
  assert(Flow6Size(table) == 2);
  Flow6Destroy(table);

  /* bounded: exact limit, updates still work, no realloc, reuse after erase */
  table = Flow6Create(500, 500, 3);
  size_t original_capacity = Flow6Capacity(table);
  for (uint32_t index = 0; index < 500; index++) {
    Flow6Key key = sMakeKey(index);
    assert(Flow6Insert(table, &key, index) == FLOW6_INSERTED);
  }
  Flow6Key extra = sMakeKey(9999);
  assert(Flow6Insert(table, &extra, 1) == FLOW6_FULL);
  Flow6Key update = sMakeKey(7);
  assert(Flow6Insert(table, &update, 77) == FLOW6_UPDATED);
  assert(Flow6Erase(table, &update));
  assert(Flow6Insert(table, &extra, 1) == FLOW6_INSERTED);
  assert(Flow6Capacity(table) == original_capacity);
  Flow6Destroy(table);

  assert(Flow6Create(SIZE_MAX, 0, 1) == NULL);
  assert(Flow6Create(1, SIZE_MAX, 1) == NULL);
  puts("ok");
  return 0;
}
