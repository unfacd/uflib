/**
 * @file flow4_test.c
 * @brief The IPv4 flow table: sequential fill, model churn, bounded behaviour.
 *
 * The table is checked first with a deterministic 500k-key fill and even-key
 * erase, which drives several growths, then against a reference model over 1M
 * random operations. The remaining checks are the details the flow table relies
 * on: the ports as separate fields, every address byte significant, the
 * reserved all-zero-ports key, the exact entry limit of a bounded table, and
 * overflow-safe creation.
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

#include <uflib/adt/robinhood_hashtable/flow4.h>

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
static Flow4Key
sMakeKey(uint32_t identifier)
{
  Flow4Key key;
  key.source_ip = 0x0A000000u + identifier;
  key.destination_ip = 0xC0A80000u ^ (identifier >> 3);
  key.source_port = (uint16_t)(1 + identifier % 65000);
  key.destination_port = (identifier % 7) ? 443 : 80;
  return key;
}

static void
sCheckSequential(void)
{
  Flow4Table *table = Flow4Create(0, 0, 9);
  for (uint32_t index = 0; index < 500000; index++) {
    Flow4Key key = { 0x0A000000u + index * 2654435761u, 0xC0A80000u ^ (index >> 3),
                     (uint16_t)(1024 + index), (uint16_t)(index % 7 ? 443 : 80) };
    assert(Flow4Insert(table, &key, index) == FLOW4_INSERTED);
  }
  for (uint32_t index = 0; index < 500000; index += 2) {
    Flow4Key key = { 0x0A000000u + index * 2654435761u, 0xC0A80000u ^ (index >> 3),
                     (uint16_t)(1024 + index), (uint16_t)(index % 7 ? 443 : 80) };
    assert(Flow4Erase(table, &key));
  }
  for (uint32_t index = 0; index < 500000; index++) {
    Flow4Key key = { 0x0A000000u + index * 2654435761u, 0xC0A80000u ^ (index >> 3),
                     (uint16_t)(1024 + index), (uint16_t)(index % 7 ? 443 : 80) };
    uint32_t value;
    bool found = Flow4Find(table, &key, &value);
    assert(found == ((index & 1) != 0) && (!found || value == index));
  }
  assert(Flow4Size(table) == 250000);
  Flow4Destroy(table);
}

int
main(void)
{
  sCheckSequential();

  /* random churn against a reference model */
  enum { MODEL = 100000 };
  static bool present[MODEL];
  static uint32_t values[MODEL];
  Flow4Table *table = Flow4Create(0, 0, 0x1234abcd);
  size_t count = 0;
  for (int round = 0; round < 1000000; round++) {
    uint32_t identifier = sRandom() % MODEL;
    Flow4Key key = sMakeKey(identifier);
    switch (sRandom() % 3) {
    case 0: {
      uint32_t value = sRandom();
      int result = Flow4Insert(table, &key, value);
      assert(result == (present[identifier] ? FLOW4_UPDATED : FLOW4_INSERTED));
      if (!present[identifier]) count++;
      present[identifier] = true;
      values[identifier] = value;
      break;
    }
    case 1: {
      bool erased = Flow4Erase(table, &key);
      assert(erased == present[identifier]);
      if (present[identifier]) count--;
      present[identifier] = false;
      break;
    }
    default: {
      uint32_t value = 0;
      bool found = Flow4Find(table, &key, &value);
      assert(found == present[identifier] && (!found || value == values[identifier]));
    }
    }
    assert(Flow4Size(table) == count);
  }

  static Flow4Key batch_keys[2048];
  static uint32_t batch_values[2048];
  static bool batch_found[2048];
  uint32_t identifiers[2048];
  for (int index = 0; index < 2048; index++) {
    identifiers[index] = sRandom() % MODEL;
    batch_keys[index] = sMakeKey(identifiers[index]);
  }
  size_t hits = Flow4FindBatch(table, batch_keys, 2048, batch_values, batch_found), expected_hits = 0;
  for (int index = 0; index < 2048; index++) {
    assert(batch_found[index] == present[identifiers[index]]);
    if (batch_found[index]) {
      assert(batch_values[index] == values[identifiers[index]]);
      expected_hits++;
    }
  }
  assert(hits == expected_hits);
  Flow4Destroy(table);

  /* the ports are distinct fields; every address byte matters */
  table = Flow4Create(0, 0, 1);
  Flow4Key first = sMakeKey(5), swapped_ports = first;
  swapped_ports.source_port = first.destination_port;
  swapped_ports.destination_port = first.source_port;
  assert(Flow4Insert(table, &first, 1) == FLOW4_INSERTED);
  assert(Flow4Insert(table, &swapped_ports, 2) == FLOW4_INSERTED);
  uint32_t value;
  assert(Flow4Find(table, &first, &value) && value == 1);
  assert(Flow4Find(table, &swapped_ports, &value) && value == 2);
  for (int byte = 0; byte < 8; byte++) {
    Flow4Key mutated = first;
    unsigned char *address = (unsigned char *)(byte < 4 ? &mutated.source_ip : &mutated.destination_ip);
    address[byte & 3] ^= 0x80;
    assert(!Flow4Find(table, &mutated, NULL));
  }

  /* the reserved key: both ports zero */
  Flow4Key reserved = { 1, 2, 0, 0 };
  assert(Flow4Insert(table, &reserved, 1) == FLOW4_ERROR);
  assert(!Flow4Find(table, &reserved, NULL));
  assert(!Flow4Erase(table, &reserved));
  assert(Flow4Size(table) == 2);
  Flow4Destroy(table);

  /* bounded: exact limit, updates still work, no realloc, reuse after erase */
  table = Flow4Create(500, 500, 3);
  size_t original_capacity = Flow4Capacity(table);
  for (uint32_t index = 0; index < 500; index++) {
    Flow4Key key = sMakeKey(index);
    assert(Flow4Insert(table, &key, index) == FLOW4_INSERTED);
  }
  Flow4Key extra = sMakeKey(9999);
  assert(Flow4Insert(table, &extra, 1) == FLOW4_FULL);
  Flow4Key update = sMakeKey(7);
  assert(Flow4Insert(table, &update, 77) == FLOW4_UPDATED);
  assert(Flow4Erase(table, &update));
  assert(Flow4Insert(table, &extra, 1) == FLOW4_INSERTED);
  assert(Flow4Capacity(table) == original_capacity);
  Flow4Destroy(table);

  assert(Flow4Create(SIZE_MAX, 0, 1) == NULL);
  assert(Flow4Create(1, SIZE_MAX, 1) == NULL);
  puts("ok");
  return 0;
}
